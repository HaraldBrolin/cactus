/*
 * phlogeny.c
 *
 *  Created on: Jun 2, 2014
 *      Author: benedictpaten
 */


#include "cactus.h"
#include "stPinchGraphs.h"
#include "stCactusGraphs.h"
#include "stPinchPhylogeny.h"
#include "stCaf.h"
#include "sonLib.h"
#include "sonLibTypes.h"

/*
 * Gets a list of the segments in the block that are part of outgroup threads.
 * The list contains stIntTuples, each of length 1, representing the index of a particular segment in
 * the block.
 */
static stList *getOutgroupThreads(stPinchBlock *block, stSet *outgroupThreads) {
    stList *outgroups = stList_construct3(0, (void (*)(void *))stIntTuple_destruct);
    stPinchBlockIt segmentIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment;
    int64_t i=0;
    while((segment = stPinchBlockIt_getNext(&segmentIt)) != NULL) {
        if(stSet_search(outgroupThreads, stPinchSegment_getThread(segment)) != NULL) {
            stList_append(outgroups, stIntTuple_construct1(i));
        }
        i++;
    }
    return outgroups;
}

/*
 * Splits the blocks using the given partition.
 */
static void splitBlock(stPinchBlock *block, stList *partitions) {
    //Build a mapping of indices of the segments in the block to the segments
    int64_t blockDegree = stPinchBlock_getDegree(block);
    stPinchSegment **segments = st_calloc(blockDegree, sizeof(stPinchSegment *));
    bool *orientations = st_calloc(blockDegree, sizeof(bool));
    stPinchBlockIt segmentIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment;
    int64_t i=0;
    while((segment = stPinchBlockIt_getNext(&segmentIt)) != NULL) {
        segments[i] = segment;
        assert(segments[i] != NULL);
        orientations[i++] = stPinchSegment_getBlockOrientation(segment);
    }
    //Destruct old block, as we build new blocks now.
    stPinchBlock_destruct(block);
    //Now build the new blocks.
    for(int64_t i=0; i<stList_length(partitions); i++) {
        stList *partition = stList_get(partitions, i);
        assert(stList_length(partition) > 0);
        int64_t k = stIntTuple_get(stList_get(partition, 0), 0);
        block = stPinchBlock_construct3(segments[k], orientations[k]);
        segments[k] = NULL; //Defensive, and used for debugging.
        for(int64_t j=1; j<stList_length(partition); j++) {
            k = stIntTuple_get(stList_get(partition, i), 0);
            stPinchBlock_pinch2(block, segments[k], orientations[k]);
            segments[k] = NULL; //Defensive, and used for debugging.
        }
    }
    //Now check the segments have all been used - this is just debugging.
    for(int64_t i=0; i<blockDegree; i++) {
        assert(segments[i] == NULL);
    }
    //Cleanup
    free(segments);
    free(orientations);
}

void stCaf_buildTreesToRemoveAncientHomologies(stPinchThreadSet *threadSet, stHash *threadStrings, stSet *outgroupThreads) {
    stPinchThreadSetBlockIt blockIt = stPinchThreadSet_getBlockIt(threadSet);
    stPinchBlock *block;

    //Hash in which we store a map of blocks to the partitions
    stHash *blocksToPartitions = stHash_construct2(NULL, NULL);

    //The loop to build a tree for each block
    while ((block = stPinchThreadSetBlockIt_getNext(&blockIt)) != NULL) {
        //Now build the tree..

        /*
         * Parameters.
         */
        int64_t maxBaseDistance = 1000;
        int64_t maxBlockDistance = 100;
        bool ignoreUnalignedBases = 1;
        bool onlyIncludeCompleteFeatureBlocks = 0;

        //Get the feature blocks
        stList *featureBlocks = stFeatureBlock_getContextualFeatureBlocks(block, maxBaseDistance, maxBlockDistance,
                ignoreUnalignedBases, onlyIncludeCompleteFeatureBlocks, threadStrings);

        //Make feature columns
        stList *featureColumns = stFeatureColumn_getFeatureColumns(featureBlocks, block);

        //Make substitution matrix
        stMatrix *substitutionMatrix = stPinchPhylogeny_getMatrixFromSubstitutions(featureColumns, block, NULL, 0);

        //Make breakpoint matrix
        stMatrix *breakpointMatrix = stPinchPhylogeny_getMatrixFromBreakpoints(featureColumns, block, NULL, 0);

        //Combine the matrices into distance matrices.
        stMatrix_scale(substitutionMatrix, 1.0, 0.0);
        stMatrix_scale(breakpointMatrix, 1.0, 0.0);
        stMatrix *combinedMatrix = stMatrix_add(substitutionMatrix, breakpointMatrix);
        stMatrix *distanceMatrix = stPinchPhylogeny_getSymmetricDistanceMatrix(combinedMatrix);

        //Build the tree...
        stTree *blockTree = stPhylogeny_neighborJoin(distanceMatrix);

        //TODO, add in features relating to bootstrapping

        //Get the outgroup threads
        stList *outgroups = getOutgroupThreads(block, outgroupThreads);

        //Get the partitions
        stList *partition = stPinchPhylogeny_splitTreeOnOutgroups(blockTree, outgroups);
        stHash_insert(blocksToPartitions, block, partition);

        //Cleanup
        stMatrix_destruct(substitutionMatrix);
        stMatrix_destruct(breakpointMatrix);
        stMatrix_destruct(combinedMatrix);
        stMatrix_destruct(distanceMatrix);
        stTree_destruct(blockTree);
        stList_destruct(featureColumns);
        stList_destruct(featureBlocks);
    }

    //Now walk through the blocks and do the actual splits, must be done after the fact using the blocks
    //in the original hash, as we are now disrupting and changing the original graph.
    stHashIterator *blockIt2 = stHash_getIterator(blocksToPartitions);
    while ((block = stHash_getNext(blockIt2)) != NULL) {
        stList *partition = stHash_search(blocksToPartitions, block);
        assert(partition != NULL);
        splitBlock(block, partition);
        stList_destruct(partition);
    }
    stHash_destructIterator(blockIt2);

    //Cleanup
    stHash_destruct(blocksToPartitions);
}
