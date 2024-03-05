#include "Alignment.h"
#include "BacktraceTranslator.h"
#include "DBReader.h"
#include "DBWriter.h"
#include "Debug.h"
#include "IndexReader.h"
#include "FoldmasonParameters.h"
#include "Matcher.h"
#include "MathUtil.h"
#include "MsaFilter.h"
#include "MultipleAlignment.h"
#include "PSSMCalculator.h"
#include "Parameters.h"
#include "Sequence.h"
#include "StructureSmithWaterman.h"
// #include "affineneedlemanwunsch.h"
#include "StructureUtil.h"
#include "Util.h"
#include "structureto3diseqdist.h"
#include <cassert>
#include <tuple>
#include <set>
#include <fstream>
#include <iostream>
#include <regex>
#include <stack>

#include "kseq.h"
#include "KSeqBufferReader.h"
#include "KSeqWrapper.h"
#include "LDDT.h"
#include "TMaligner.h"
#include "Coordinate16.h"
#include "msa2lddt.h"

#include "refinemsa.h"
#include "structuremsa.h"
#include "newick.h"

#ifdef OPENMP
#include <omp.h>
#endif

KSEQ_INIT(kseq_buffer_t*, kseq_buffer_reader)

#define	EXIT_FAILURE	1
#define	EXIT_SUCCESS	0

Matcher::result_t pairwiseAlignment(
    StructureSmithWaterman & aligner,
    unsigned int querySeqLen,
    Sequence *query_aa,
    Sequence *query_3di,
    Sequence *target_aa,
    Sequence *target_3di,
    int gapOpen,
    int gapExtend,
    SubstitutionMatrix *mat_aa,
    SubstitutionMatrix *mat_3di,
    int compBiasCorrection
) {
    std::string backtrace;

    bool targetIsProfile = (Parameters::isEqualDbtype(target_aa->getSeqType(), Parameters::DBTYPE_HMM_PROFILE));
    bool queryIsProfile = (Parameters::isEqualDbtype(query_aa->getSeqType(), Parameters::DBTYPE_HMM_PROFILE));

    unsigned char * query_aa_seq = query_aa->numSequence;
    unsigned char * query_3di_seq = query_3di->numSequence;
    unsigned char * target_aa_seq = target_aa->numSequence;
    unsigned char * target_3di_seq = target_3di->numSequence;
    if (queryIsProfile) {
        query_aa_seq = query_aa->numConsensusSequence;
        query_3di_seq = query_3di->numConsensusSequence;
    }
    if (targetIsProfile) {
        target_aa_seq = target_aa->numConsensusSequence;
        target_3di_seq = target_3di->numConsensusSequence;
    }

    float *composition_bias_aa  = new float[query_aa->L];
    float *composition_bias_ss  = new float[query_aa->L];
    float *tmp_composition_bias = new float[query_aa->L];
    if (compBiasCorrection) {
        SubstitutionMatrix::calcLocalAaBiasCorrection(mat_aa, query_aa->numSequence, query_aa->L, tmp_composition_bias, 1.0);
        for (int i =0; i < query_aa->L; i++) {
            composition_bias_aa[i] = (int8_t) (tmp_composition_bias[i] < 0.0) ? tmp_composition_bias[i] - 0.5 : tmp_composition_bias[i] + 0.5;
        }
        SubstitutionMatrix::calcLocalAaBiasCorrection(mat_3di, query_3di->numSequence, query_3di->L, tmp_composition_bias, 1.0);
        for (int i =0; i < query_aa->L; i++) {
            composition_bias_ss[i] = (int8_t) (tmp_composition_bias[i] < 0.0) ? tmp_composition_bias[i] - 0.5 : tmp_composition_bias[i] + 0.5;
        }
    } else {
        memset(composition_bias_aa, 0, query_aa->L * sizeof(int8_t));
        memset(composition_bias_ss, 0, query_aa->L * sizeof(int8_t));
    }

    short **query_profile_scores_aa = new short * [aligner.get_profile()->alphabetSize];
    short **query_profile_scores_3di = new short * [aligner.get_profile()->alphabetSize];
    for (int32_t j = 0; j < aligner.get_profile()->alphabetSize; j++) {
        query_profile_scores_aa[j] = new short [querySeqLen];
        query_profile_scores_3di[j] = new short [querySeqLen];
    }
    if (queryIsProfile) {
        for (unsigned int i = 0; i < querySeqLen; i++) {
            for (int32_t j = 0; j < aligner.get_profile()->alphabetSize; j++) {
                query_profile_scores_aa[j][i]  = query_aa->profile_for_alignment[j * querySeqLen + i];
                query_profile_scores_3di[j][i] = query_3di->profile_for_alignment[j * querySeqLen + i];
            }
        }
    } else {
        for (unsigned int i = 0; i < querySeqLen; i++) {
            for (int32_t j = 0; j < aligner.get_profile()->alphabetSize; j++) {
                query_profile_scores_aa[j][i]  = mat_aa->subMatrix[j][query_aa_seq[i]]   + composition_bias_aa[i];
                query_profile_scores_3di[j][i] = mat_3di->subMatrix[j][query_3di_seq[i]] + composition_bias_ss[i];
            }
        }
    }
   
    short **target_profile_scores_aa = new short * [aligner.get_profile()->alphabetSize];
    short **target_profile_scores_3di = new short * [aligner.get_profile()->alphabetSize];
    for (int32_t j = 0; j < aligner.get_profile()->alphabetSize; j++) {
        target_profile_scores_aa[j]  = new short [target_aa->L];
        target_profile_scores_3di[j] = new short [target_aa->L];
    }
    if (targetIsProfile) {
        for (int i = 0; i < target_aa->L; i++) {
            for (int32_t j = 0; j < aligner.get_profile()->alphabetSize; j++) {
                target_profile_scores_aa[j][i]  = target_aa->profile_for_alignment[j * target_aa->L + i];
                target_profile_scores_3di[j][i] = target_3di->profile_for_alignment[j * target_aa->L + i];
            }
        }
    } else {
        for (int i = 0; i < target_aa->L; i++) {
            for (int32_t j = 0; j < aligner.get_profile()->alphabetSize; j++) {
                target_profile_scores_aa[j][i]  = mat_aa->subMatrix[j][target_aa_seq[i]];
                target_profile_scores_3di[j][i] = mat_3di->subMatrix[j][target_3di_seq[i]];
            }
        }
    }

    delete[] composition_bias_aa;
    delete[] composition_bias_ss;
    delete[] tmp_composition_bias;

    Matcher::result_t gAlign = aligner.simpleGotoh(
        target_aa_seq,
        target_3di_seq,
        query_profile_scores_aa,
        query_profile_scores_3di,
        target_profile_scores_aa,
        target_profile_scores_3di,
        0,
        query_aa->L,
        0,
        target_aa->L,
        gapOpen,
        gapExtend
    );

    for (int32_t i = 0; i < aligner.get_profile()->alphabetSize; i++) {
        delete[] query_profile_scores_aa[i];
        delete[] query_profile_scores_3di[i];
        delete[] target_profile_scores_aa[i];
        delete[] target_profile_scores_3di[i];
    }
    
    delete[] query_profile_scores_aa;
    delete[] query_profile_scores_3di;
    delete[] target_profile_scores_aa;
    delete[] target_profile_scores_3di;
   
    return gAlign;
}

void sortHitsByScore(std::vector<AlnSimple> &hits) {
    SORT_PARALLEL(hits.begin(), hits.end(), [](const AlnSimple & a, const AlnSimple & b) {
        // sort by score then qId then tId
        if (a.score == b.score) {
            if (a.queryId == b.queryId) {
                return a.targetId < b.targetId;
            }
            return a.queryId < b.queryId;
        }
        return a.score > b.score;
    });
}

std::vector<AlnSimple> removeMergedHits(std::vector<AlnSimple> & hits, size_t mergedId, size_t targetId) {
    std::vector<AlnSimple> newHits;
    for (size_t i = 0; i < hits.size(); i++) {
        if (hits[i].queryId != mergedId && hits[i].targetId != mergedId
            && hits[i].queryId != targetId && hits[i].targetId != targetId) {
            newHits.push_back(hits[i]);
        }
    }
    return newHits;
}


inline size_t get1dIndex(size_t i, size_t j, size_t N) {
    return j + i * (2 * N - i - 1) / 2 - i - 1;
}

std::vector<AlnSimple> updateAllScores(
    DBReader<unsigned int> &seqDbrAA,
    DBReader<unsigned int> &seqDbr3Di,
    int8_t * tinySubMatAA,
    int8_t * tinySubMat3Di,
    SubstitutionMatrix * subMat_aa,
    SubstitutionMatrix * subMat_3di,
    bool * alreadyMerged,
    int maxSeqLen,
    int alphabetSize,
    int compBiasCorrection,
    int compBiasCorrectionScale
) {
    std::vector<AlnSimple> newHits;
    size_t sequenceCnt = seqDbrAA.getSize();

#pragma omp parallel
{

    unsigned int thread_idx = 0;
#ifdef OPENMP
    thread_idx = static_cast<unsigned int>(omp_get_thread_num());
#endif

    Sequence seqMergedAa(maxSeqLen, Parameters::DBTYPE_AMINO_ACIDS, (const BaseMatrix *) subMat_aa,  0, false, compBiasCorrection);
    Sequence seqMergedSs(maxSeqLen, Parameters::DBTYPE_AMINO_ACIDS, (const BaseMatrix *) subMat_3di, 0, false, compBiasCorrection);
    Sequence seqTargetAa(maxSeqLen, Parameters::DBTYPE_AMINO_ACIDS, (const BaseMatrix *) subMat_aa,  0, false, compBiasCorrection);
    Sequence seqTargetSs(maxSeqLen, Parameters::DBTYPE_AMINO_ACIDS, (const BaseMatrix *) subMat_3di, 0, false, compBiasCorrection);

    StructureSmithWaterman structureSmithWaterman(
        maxSeqLen,
        alphabetSize,
        compBiasCorrection,
        compBiasCorrectionScale,
        subMat_aa,
        subMat_3di
    );
    std::vector<AlnSimple> threadHits;

#pragma omp for schedule(dynamic, 10)
    for (unsigned int i = 0; i < sequenceCnt; i++) {
        if (alreadyMerged[i])
            continue;

        unsigned int mergedKey = seqDbrAA.getDbKey(i);
        size_t mergedId  = seqDbrAA.getId(mergedKey);
        seqMergedAa.mapSequence(mergedId, mergedKey, seqDbrAA.getData(mergedId, thread_idx), seqDbrAA.getSeqLen(mergedId));
        mergedId = seqDbr3Di.getId(mergedKey);
        seqMergedSs.mapSequence(mergedId, mergedKey, seqDbr3Di.getData(mergedId, thread_idx), seqDbr3Di.getSeqLen(mergedId));

        structureSmithWaterman.ssw_init(
            &seqMergedAa,
            &seqMergedSs,
            tinySubMatAA,
            tinySubMat3Di,
            subMat_aa
        );

        for (size_t j = i + 1; j < sequenceCnt; j++) {
            if (alreadyMerged[j] || i == j)
                continue;

            size_t targetKey = seqDbrAA.getDbKey(j);
            size_t targetId  = seqDbrAA.getId(targetKey);
            seqTargetAa.mapSequence(targetId, targetKey, seqDbrAA.getData(targetId, i), seqDbrAA.getSeqLen(targetId));
            seqTargetSs.mapSequence(targetId, targetKey, seqDbr3Di.getData(targetId, i), seqDbr3Di.getSeqLen(targetId));

            AlnSimple aln;
            aln.queryId = mergedId;
            aln.targetId = targetId;
            aln.score = structureSmithWaterman.ungapped_alignment(seqTargetAa.numSequence, seqTargetSs.numSequence, seqTargetAa.L);
            threadHits.push_back(aln); 
        }
    }

#pragma omp critical
    {
        newHits.insert(newHits.end(), threadHits.begin(), threadHits.end());
    }
}
    return newHits;
}

int findRoot(int vertex, std::vector<int>& parent) {
    while (parent[vertex] != vertex) {
        parent[vertex] = parent[parent[vertex]];
        vertex = parent[vertex];
    }
    return vertex;
}

/**
 * @brief Get minimum spanning tree as linkage matrix (Kruskal algorithm).
 * 
 * @param hits all hits from UpdateAllScores
 * @param n number of structures
 * @return std::vector<AlnSimple> 
 */
std::vector<AlnSimple> mst(std::vector<AlnSimple> hits, int n) {
    std::vector<AlnSimple> result;
    std::vector<int> parent(n);  // parent node IDs
    for (int i = 0; i < n; i++)
        parent[i] = i;
    for (AlnSimple aln : hits) {
        int u = findRoot(aln.queryId, parent);
        int v = findRoot(aln.targetId, parent);
        if (u != v) {
            result.push_back(aln);
            parent[u] = v;
        }
    }
    return result;
}

/**
 * @brief Reorder linkage matrix to maximize unique merges per iteration for multithreading.
 * 
 * @param linkage linkage matrix generated by `mst`
 * @param merges number of unique merges per iteration
 * @param n number of structures
 * @return std::vector<AlnSimple> 
 */
std::vector<AlnSimple> reorderLinkage(std::vector<AlnSimple> linkage, std::vector<size_t> &merges, int n) {
    std::vector<int> parent(n); 
    std::vector<int> counts(n);
    for (int i = 0; i < n; i++) {
        parent[i] = i;
        counts[i] = 0;
    }
    std::vector<AlnSimple> result(linkage.size());
    std::vector<bool> merged(linkage.size());
    int index = 0;
    int mergeCount = 0; // no. total merges
    int mergeTally = 0; // count per round
    while (mergeCount < (int)linkage.size()) {
        for (int i = 0; i < n; i++)
            counts[i] = 0;
        for (size_t i = 0; i < linkage.size(); i++) {
            if (merged[i])
                continue;
            AlnSimple aln = linkage[i];
            int u = findRoot(aln.queryId, parent);
            int v = findRoot(aln.targetId, parent);
            if (counts[u] > 0 || counts[v] > 0)
                continue;
            result[index++] = aln;
            parent[u] = v;
            merged[i] = true;
            counts[u]++;
            counts[v]++;
            mergeTally++;
        }
        merges.push_back(mergeTally);
        mergeCount += mergeTally;
        mergeTally = 0;
    }
    return result;
}

int cigarLength(std::vector<Instruction> &cigar, bool withGaps) {
    int count = 0;
    for (Instruction ins : cigar) {
        count += (ins.isSeq()) ? 1 : (withGaps ? static_cast<int>(ins.bits.count) : 0);
    }
    return count;
}

/**
 * @brief Compute MSA mask based on sequence weights
 *
 * "Position-based Sequence Weights", Henikoff (1994)
 * 
 * @param indices indices of structures in this MSA
 * @param cigars all structure instruction vectors
 * @param lengths all structure lengths
 * @param lengthWithGaps gappy alignment length
 * @return std::vector<float> 
 */
std::string computeProfileMask(
    std::vector<size_t> &indices,
    std::vector<std::vector<Instruction> > &cigars,
    std::vector<int> &lengths,
    SubstitutionMatrix &subMat,
    float matchRatio
) {
    int lengthWithGaps = cigarLength(cigars[indices[0]], true);

    // initialise weights with tiny pseudo counts
    std::vector<float> seqWeights(indices.size(), 1e-6);
    
    // count residues at each position of the alignment
    // 0-19 residue types
    // 20   number of distinct residues
    std::vector<unsigned int> counts((Sequence::PROFILE_AA_SIZE + 1) * lengthWithGaps, 0);
    for (size_t i = 0; i < indices.size(); i++) {
        int cigIndex = indices[i];
        int seqIndex = 0;
        for (size_t j = 0; j < cigars[cigIndex].size(); j++) {
            Instruction ins = cigars[cigIndex][j];
            if (ins.isSeq()) {
                const unsigned int c  = subMat.aa2num[static_cast<int>(ins.getCharacter())];
                if (c < Sequence::PROFILE_AA_SIZE) {  // ignore X (20)
                    int ij = c * lengthWithGaps + seqIndex;
                    counts[ij] += 1;
                    if (counts[ij] == 1) {
                        counts[(Sequence::PROFILE_AA_SIZE) * lengthWithGaps + seqIndex]++;
                    }
                }
                seqIndex++;
            } else {
                seqIndex += ins.bits.count; 
            }
        }
    }
    
    // running sums of seq weights for matches/gaps per column of alignment per sequence
    // 0 matches
    // 1 gaps
    std::vector<float> colValues(2 * lengthWithGaps, 0.0);
   
    for (size_t i = 0; i < indices.size(); i++) {
        int cigIndex = indices[i];
        int seqIndex = 0;

        // Compute sequence weights
        for (Instruction &ins : cigars[cigIndex]) {
            if (ins.isSeq()) {
                const unsigned int c = subMat.aa2num[static_cast<int>(ins.getCharacter())];
                int distinct = counts[(Sequence::PROFILE_AA_SIZE) * lengthWithGaps + seqIndex];
                int ij = c * lengthWithGaps + seqIndex;
                if (counts[ij] > 0 && distinct > 0) {
                    seqWeights[i] += 1.0f / (
                        static_cast<float>(counts[ij])
                        * static_cast<float>(distinct)
                        * (static_cast<float>(lengths[cigIndex]) + 30.0f)
                    );
                }
                seqIndex++;
            } else {
                seqIndex += ins.bits.count; 
            }
        }
        
        // Add weights for this sequence to matches/gaps per column
        seqIndex = 0;
        for (size_t j = 0; j < cigars[cigIndex].size(); j++) {
            Instruction &ins = cigars[cigIndex][j];
            if (ins.isSeq()) {
                const unsigned int c = subMat.aa2num[static_cast<int>(ins.getCharacter())];
                if (c < Sequence::PROFILE_AA_SIZE) {
                    colValues[seqIndex] += seqWeights[i];
                } 
                seqIndex++;
            } else {
                // ignore end gaps
                if (j != 0 && (j != cigars[cigIndex].size() - 1)) {
                    for (int k = 0; k < ins.bits.count; k++) {
                        colValues[lengthWithGaps + seqIndex + k] = seqWeights[i];
                    }
                }
                seqIndex += ins.bits.count;
            }
        }
    }
    
    // Generate mask string
    std::string mask;
    for (int i = 0; i < lengthWithGaps; i++) {
        float matches = colValues[i];
        float gaps = colValues[lengthWithGaps + i];
        bool state = (gaps / (gaps + matches)) >= matchRatio;
        mask.push_back(state ? '1' : '0');
    }

    return mask;
}

std::vector<int> parseQidString(std::string qid) {
    std::vector<std::string> qid_str_vec = Util::split(qid, ",");
    std::vector<int> qid_vec;
    for (size_t qid_idx = 0; qid_idx < qid_str_vec.size(); qid_idx++) {
        float qid_float = strtod(qid_str_vec[qid_idx].c_str(), NULL);
        qid_vec.push_back(static_cast<int>(qid_float*100));
    }
    std::sort(qid_vec.begin(), qid_vec.end());
    return qid_vec;
}

// Generate PSSM from CIGARs and a MSA mask
std::string msa2profile(
    std::vector<size_t> &indices,
    std::vector<std::vector<Instruction> > &cigars,
    std::string mask,
    PSSMCalculator &pssmCalculator,
    MsaFilter &filter,
    SubstitutionMatrix &subMat,
    bool filterMsa,
    bool compBiasCorrection,
    std::string & qid,
    float filterMaxSeqId,
    float Ndiff,
    float covMSAThr,
    float qsc,
    int filterMinEnable,
    bool wg
) {
    // length of sequences after masking
    int lengthWithMask = 0;
    for (char c : mask) {
        if (c == '0') lengthWithMask++;
    }

    float *pNullBuffer = new float[lengthWithMask];

    // build reduced MSA
    char **msaSequences = MultipleAlignment::initX(lengthWithMask + 1, indices.size());
    for (size_t i = 0; i < indices.size(); i++) {
        msaSequences[i][lengthWithMask] = '\0';
        int seqIndex = 0;
        int msaIndex = 0;
        for (Instruction &ins : cigars[indices[i]]) {
            if (ins.isSeq()) {
                const unsigned int c = subMat.aa2num[static_cast<int>(ins.getCharacter())];
                if (mask[seqIndex] == '0') {
                    msaSequences[i][msaIndex] = c;
                    msaIndex++;
                }
                seqIndex++;
            } else {
                for (size_t j = 0; j < ins.bits.count; j++) {
                    if (mask[seqIndex] == '0') {
                        msaSequences[i][msaIndex] = (int)MultipleAlignment::GAP;
                        msaIndex++;
                    }
                    seqIndex++;
                }
            }
        }
        assert(msaIndex == lengthWithMask);
    }
    
    MultipleAlignment::MSAResult msaResult(lengthWithMask, lengthWithMask, indices.size(), msaSequences);

    size_t filteredSetSize = indices.size();
    if (filterMsa == 1) {
        std::vector<int> qid_vec = parseQidString(qid);
        filteredSetSize = filter.filter(
            indices.size(),
            lengthWithMask,
            static_cast<int>(covMSAThr * 100),
            qid_vec,
            qsc,
            static_cast<int>(filterMaxSeqId * 100),
            Ndiff,
            filterMinEnable,
            (const char **) msaSequences,
            true
        );
    }

    PSSMCalculator::Profile pssmRes = pssmCalculator.computePSSMFromMSA(
        filteredSetSize,
        msaResult.centerLength,
        (const char **) msaResult.msaSequence,
#ifdef GAP_POS_SCORING
        alnResults,
#endif
        wg,
        // FIXME
        0.6
    );
    
    if (compBiasCorrection) {
        SubstitutionMatrix::calcGlobalAaBiasCorrection(
            &subMat,
            pssmRes.pssm,
            pNullBuffer,
            Sequence::PROFILE_AA_SIZE,
            lengthWithMask
        );
    }
    unsigned char * consensus = new unsigned char[lengthWithMask];
    for (int i = 0; i < lengthWithMask; ++i)
        consensus[i] = subMat.aa2num[pssmRes.consensus[i]];
    std::string result;
    pssmRes.toBuffer(consensus, lengthWithMask, subMat, result);

    delete[] pNullBuffer;
    free(msaSequences[0]);
    delete[] msaSequences;
    delete[] consensus;
    
    return result;
}

// Map 0001100 to [ 0 1 2 5 6 ]
// needs to be ungapped->gapped direction
std::vector<int> maskToMapping(std::string mask) {
    std::vector<int> mapping;
    for (size_t i = 0; i < mask.length(); ++i) {
        if (mask[i] == '0')
            mapping.push_back(i);
    }
    return mapping;
}

std::vector<AlnSimple> parseAndScoreExternalHits(
    DBReader<unsigned int> &seqDbrAA,
    DBReader<unsigned int> &seqDbr3Di,
    DBReader<unsigned int> *cluDbr,
    int8_t * tinySubMatAA,
    int8_t * tinySubMat3Di,
    SubstitutionMatrix * subMat_aa,
    SubstitutionMatrix * subMat_3di,
    int maxSeqLen,
    int alphabetSize,
    int compBiasCorrection,
    int compBiasCorrectionScale
) {
    // open an alignment DBReader
    std::vector<AlnSimple> allAlnResults;

#pragma omp parallel
{
    unsigned int thread_idx = 0;
#ifdef OPENMP
    thread_idx = static_cast<unsigned int>(omp_get_thread_num());
#endif

    Sequence seqQueryAa(maxSeqLen, Parameters::DBTYPE_AMINO_ACIDS, (const BaseMatrix *) subMat_aa,  0, false, compBiasCorrection);
    Sequence seqQuerySs(maxSeqLen, Parameters::DBTYPE_AMINO_ACIDS, (const BaseMatrix *) subMat_3di, 0, false, compBiasCorrection);
    Sequence seqDbAa(maxSeqLen, Parameters::DBTYPE_AMINO_ACIDS, (const BaseMatrix *) subMat_aa,  0, false, compBiasCorrection);
    Sequence seqDbSs(maxSeqLen, Parameters::DBTYPE_AMINO_ACIDS, (const BaseMatrix *) subMat_3di, 0, false, compBiasCorrection);

    StructureSmithWaterman structureSmithWaterman(
        maxSeqLen,
        alphabetSize,
        compBiasCorrection,
        compBiasCorrectionScale,
        subMat_aa,
        subMat_3di
    );
    std::vector<AlnSimple> threadAlnResults;
    char buffer[255 + 1];

#pragma omp for schedule(dynamic, 10)
    for (size_t i = 0; i < cluDbr->getSize(); ++i) {
        char *data = cluDbr->getData(i, thread_idx);
        unsigned int queryKey = cluDbr->getDbKey(i);
        
        size_t queryId = seqDbrAA.getId(queryKey);
        seqQueryAa.mapSequence(queryId, queryKey, seqDbrAA.getData(queryId, thread_idx), seqDbrAA.getSeqLen(queryId));
        queryId = seqDbr3Di.getId(queryKey);
        seqQuerySs.mapSequence(queryId, queryKey, seqDbr3Di.getData(queryId, thread_idx), seqDbr3Di.getSeqLen(queryId));
        
        structureSmithWaterman.ssw_init(
            &seqQueryAa,
            &seqQuerySs,
            tinySubMatAA,
            tinySubMat3Di,
            subMat_aa
        );
       
        while (*data != '\0') {
            Util::parseKey(data, buffer);
            const unsigned int dbKey = (unsigned int) strtoul(buffer, NULL, 10);
            if (queryKey == dbKey) {
                data = Util::skipLine(data);
                continue;
            }
            size_t dbId = seqDbrAA.getId(dbKey);
            seqDbAa.mapSequence(dbId, dbKey, seqDbrAA.getData(dbId, thread_idx), seqDbrAA.getSeqLen(dbId));
            dbId = seqDbr3Di.getId(dbKey);
            seqDbSs.mapSequence(dbId, dbKey, seqDbr3Di.getData(dbId, thread_idx), seqDbr3Di.getSeqLen(dbId));
            AlnSimple aln;
            aln.queryId = queryKey;
            aln.targetId = dbKey;
            aln.score = structureSmithWaterman.ungapped_alignment(
                seqDbAa.numSequence,
                seqDbSs.numSequence,
                seqDbAa.L
            );
            threadAlnResults.push_back(aln);
            data = Util::skipLine(data);
        }
    }
#pragma omp critical
    {
        allAlnResults.insert(allAlnResults.end(), threadAlnResults.begin(), threadAlnResults.end());
    }
}
    return allAlnResults;
}

void addCigarStates(std::vector<Instruction> &cigar, int state, int count) {
    while (count > 0) {
        if (cigar.empty() || cigar.back().bits.state != state || cigar.back().isFull()) {
            cigar.emplace_back(state, 0); 
        }
        int spaceLeft = 127 - static_cast<int>(cigar.back().bits.count);
        if (count > spaceLeft) {
            cigar.back().bits.count = 127;
            count -= spaceLeft;
        } else {
            cigar.back().bits.count += count;
            count = 0;
        }
    }
}

/**
 * @brief Get merge instructions for two MSAs
 * 
 * @param res  - alignment result
 * @param map1 - ungapped->gapped mapping for msa1
 * @param map2 - ungapped->gapped mapping for msa2
 * @param qBt  - vector to store query merge instructions
 * @param tBt  - vector to store target merge instructions
 */
void getMergeInstructions(
    Matcher::result_t &res,
    std::vector<int> &map1,
    std::vector<int> &map2,
    std::vector<Instruction> &qBt,
    std::vector<Instruction> &tBt
) {
    qBt.emplace_back(SEQ, 1);  // first match
    tBt.emplace_back(SEQ, 1);
    int new_q, dq;
    int new_t, dt;
    int old_q = map1[res.qStartPos];
    int old_t = map2[res.dbStartPos];
    int q = res.qStartPos + 1;  // indices in non-gappy sequence
    int t = res.dbStartPos + 1;
 
    // Generate instructions for query/target sequences from backtrace
    for (size_t i = 1; i < res.backtrace.length(); ++i) {
        switch (res.backtrace[i]) {
            case 'M': {
                new_q = map1[q];
                new_t = map2[t];
                dq = new_q - old_q;
                dt = new_t - old_t; 
                if (dq == 0) {
                    // No matches in query
                    addCigarStates(qBt, GAP, dt);
                    addCigarStates(tBt, SEQ, dt);
                } else if (dq == 1) {
                    // One match in query
                    if ((dt - 1) > 0)
                        addCigarStates(qBt, GAP, dt - 1);
                    addCigarStates(qBt, SEQ, 1);
                    addCigarStates(tBt, SEQ, dt);
                } else if (dq >= dt) {
                    // More query matches than target
                    addCigarStates(qBt, SEQ, dq);
                    addCigarStates(tBt, GAP, dq - dt);
                    addCigarStates(tBt, SEQ, dt);
                } else if (dt > dq) {
                    // More target than query
                    addCigarStates(qBt, GAP, dt - dq);
                    addCigarStates(qBt, SEQ, dq);
                    addCigarStates(tBt, SEQ, dt);
                }
                old_q = new_q;
                old_t = new_t;
                ++q;
                ++t;
                break;
            }
            case 'I': {
                ++q;
                break;
            }
            case 'D': {
                ++t;
                break;
            }
        }
    }
}

inline bool needNewInstruction(std::vector<Instruction> &instructions) {
    return (
        instructions.empty()
        || instructions.back().isSeq()
        || instructions.back().isFull()
    );
}

/**
 * @brief Expands a sequence based on CIGAR
 * 
 * @param instructions Vector of Instructions
 * @return std::string Expanded alignment string
 */
std::string expand(std::vector<Instruction> &instructions) {
    std::string result = "";
    for (Instruction &ins : instructions) {
        if (ins.isSeq()) {
            result.append(1, ins.getCharacter());
        } else {
            result.append(static_cast<int>(ins.bits.count), '-');
        }
    }
    return result;
}

/**
 * @brief Convert sequence string to vector of Instructions
 * 
 * e.g. --AB-C
 *      state 1, count 2
 *      state 0, A
 *      state 0, B
 *      state 1, count 1
 *      state 0, C
 *
 * @param sequence 
 * @return std::vector<Instruction> 
 */
std::vector<Instruction> contract(std::string sequence) {
    std::vector<Instruction> instructions;
    for (char &letter : sequence) {
        if (letter == '\0') {
            break;
        }
        if (letter == '-') {
            if (needNewInstruction(instructions)) {
                instructions.emplace_back(static_cast<int>(1));
            } else {
                instructions.back().bits.count++;
            }
        } else {
            instructions.emplace_back(letter);
        }
    };
    std::string rex = expand(instructions);
    assert(rex == sequence);
    return instructions;
}

void printInstructions(std::vector<Instruction> &instructions) {
    for (Instruction ins : instructions) {
        if (ins.bits.state == 0) { 
            std::cout << ins.getCharacter();
        } else {
            for (int i = 0; i < ins.bits.count; i++) {
                std::cout << '-';
            }
        }
    }
    std::cout << '\n';
}

/**
 * @brief Add gaps to a vector of instructions
 * 
 * @param toAdd number of gaps to add to end of instructions
 * @param instructions vector of instructions
 */
void addCigarGaps(
    int toAdd,
    std::vector<Instruction> &instructionsAA,
    std::vector<Instruction> &instructionsSS
) {
    while (toAdd > 0) {
        if (needNewInstruction(instructionsAA)) {
            instructionsAA.emplace_back(0);
            instructionsSS.emplace_back(0);
        }
        int spaceLeft = 127 - static_cast<int>(instructionsAA.back().bits.count);
        if (toAdd > spaceLeft) {
            instructionsAA.back().bits.count = 127;
            instructionsSS.back().bits.count = 127;
            toAdd -= spaceLeft;
        } else {
            instructionsAA.back().bits.count += toAdd;
            instructionsSS.back().bits.count += toAdd;
            toAdd = 0;
        }
    }
}

/**
 * @brief Generate new instructions for gaps/sequence before start of alignment
 * 
 * @param toAdd number of sequence positions to add
 * @param oldIndex index of current old instruction
 * @param newInstructionsAA 
 * @param newInstructionsSS 
 * @param oldInstructionsAA 
 * @param oldInstructionsSS 
 */
void addCigarIndices(
    int toAdd,
    int &oldIndex,
    std::vector<Instruction> &newInstructionsAA,
    std::vector<Instruction> &newInstructionsSS,
    std::vector<Instruction> &oldInstructionsAA,
    std::vector<Instruction> &oldInstructionsSS
) {
    while (toAdd > 0) {
        if (oldInstructionsAA[oldIndex].isSeq()) {
            newInstructionsAA.emplace_back(oldInstructionsAA[oldIndex].getCharacter());
            newInstructionsSS.emplace_back(oldInstructionsSS[oldIndex].getCharacter());
            oldIndex++;
            toAdd--;
        } else {
            if (needNewInstruction(newInstructionsAA)) {
                newInstructionsAA.emplace_back(0);
                newInstructionsSS.emplace_back(0);
            }
            int spaceLeft = 127 - newInstructionsAA.back().bits.count;
            if (toAdd > oldInstructionsAA[oldIndex].bits.count) {
                // use ALL of this instructions count
                // just have to check space in the new count

                if (oldInstructionsAA[oldIndex].bits.count > spaceLeft) {
                    // make new instruction/s until we exhaust the old instruction
                    newInstructionsAA.back().bits.count = 127;
                    newInstructionsSS.back().bits.count = 127;
                    oldInstructionsAA[oldIndex].bits.count -= spaceLeft;
                    oldInstructionsSS[oldIndex].bits.count -= spaceLeft;
                    toAdd -= spaceLeft;
                } else {
                    newInstructionsAA.back().bits.count += oldInstructionsAA[oldIndex].bits.count;
                    newInstructionsSS.back().bits.count += oldInstructionsSS[oldIndex].bits.count;
                    toAdd -= oldInstructionsAA[oldIndex].bits.count;
                    oldIndex++;
                }
            } else {
                if (toAdd > spaceLeft) {
                    newInstructionsAA.back().bits.count = 127;
                    newInstructionsSS.back().bits.count = 127;
                    oldInstructionsAA[oldIndex].bits.count -= spaceLeft;
                    oldInstructionsSS[oldIndex].bits.count -= spaceLeft;
                    toAdd -= spaceLeft;
                } else {
                    newInstructionsAA.back().bits.count += toAdd;
                    newInstructionsSS.back().bits.count += toAdd;
                    oldInstructionsAA[oldIndex].bits.count -= toAdd;
                    oldInstructionsSS[oldIndex].bits.count -= toAdd;
                    toAdd = 0;
                }
            }
        }
    }
}

void updateQueryCIGAR(
    std::vector<Instruction> &cigar_aa,
    std::vector<Instruction> &cigar_ss,
    std::vector<Instruction> &instructions,
    int preGap,
    int preSequence,
    int endGap,
    int endSequence
) {
    int cigarIndex = 0;
    std::vector<Instruction> aa;
    std::vector<Instruction> ss;
    addCigarGaps(preGap, aa, ss);
    addCigarIndices(preSequence, cigarIndex, aa, ss, cigar_aa, cigar_ss);
    for (Instruction ins : instructions) {
        if (ins.isSeq()) {
            addCigarIndices(ins.bits.count, cigarIndex, aa, ss, cigar_aa, cigar_ss);
        } else {
            addCigarGaps(ins.bits.count, aa, ss);
        }
    }
    addCigarIndices(endSequence, cigarIndex, aa, ss, cigar_aa, cigar_ss);
    addCigarGaps(endGap, aa, ss);
    std::swap(cigar_aa, aa);
    std::swap(cigar_ss, ss);
}

void updateTargetCIGAR(
    std::vector<Instruction> &cigar_aa,
    std::vector<Instruction> &cigar_ss,
    std::vector<Instruction> &instructions,
    int preGap,
    int preSequence,
    int endGap,
    int endSequence
) {
    int cigarIndex = 0;
    std::vector<Instruction> aa;
    std::vector<Instruction> ss;
    addCigarIndices(preSequence, cigarIndex, aa, ss, cigar_aa, cigar_ss);
    addCigarGaps(preGap, aa, ss);
    for (Instruction ins : instructions) {
        if (ins.isSeq()) {
            addCigarIndices(ins.bits.count, cigarIndex, aa, ss, cigar_aa, cigar_ss);
        } else {
            addCigarGaps(ins.bits.count, aa, ss);
        }
    }
    addCigarGaps(endGap, aa, ss);
    addCigarIndices(endSequence, cigarIndex, aa, ss, cigar_aa, cigar_ss);
    std::swap(cigar_aa, aa);
    std::swap(cigar_ss, ss);
}

/**
 * @brief 
 * 
 * @param group1 structure indices in first cluster
 * @param group2 structure indices in second cluster
 * @param cigars_aa AA CIGAR instruction vectors
 * @param cigars_ss 3Di CIGAR instruction vectors
 * @param res Alignment result
 * @param map1 Mask map for query profile
 * @param map2 Mask map for target profile
 * @param qBt Merge instructions for query profile
 * @param tBt Merge instructions for target profile
 * @return std::pair<std::string, std::string> 
 */
void updateCIGARS(
    std::vector<size_t> &group1,
    std::vector<size_t> &group2,
    std::vector<std::vector<Instruction> > &cigars_aa,
    std::vector<std::vector<Instruction> > &cigars_ss,
    Matcher::result_t &res,
    std::vector<int> qMap,
    std::vector<int> tMap,
    std::vector<Instruction> &qBt,
    std::vector<Instruction> &tBt
) {
    int qPreSequence = qMap[res.qStartPos];
    int qPreGaps     = tMap[res.dbStartPos];
    int qEndSequence = qMap.back() - qMap[res.qEndPos];
    int qEndGaps     = tMap.back() - tMap[res.dbEndPos];
    int tPreSequence = qPreGaps;
    int tPreGaps     = qPreSequence;
    int tEndSequence = qEndGaps;
    int tEndGaps     = qEndSequence;
    for (size_t index : group1)
        updateQueryCIGAR(cigars_aa[index], cigars_ss[index], qBt, qPreGaps, qPreSequence, qEndGaps, qEndSequence);
    for (size_t index : group2)
        updateTargetCIGAR(cigars_aa[index], cigars_ss[index], tBt, tPreGaps, tPreSequence, tEndGaps, tEndSequence);
}

void testSeqLens(std::vector<size_t> &MAYBE_UNUSED(indices), std::vector<std::vector<Instruction> > &MAYBE_UNUSED(cigars), std::vector<int> &MAYBE_UNUSED(lengths)) {
    for (int MAYBE_UNUSED(index) : indices) {
        assert(lengths[index] == cigarLength(cigars[index], false));
    }
}

Matcher::result_t pairwiseTMAlign(
    int mergedId,
    int targetId,
    DBReader<unsigned int> &seqDbrAA,
    DBReader<unsigned int> &seqDbrCA
) {
    int qLen = seqDbrAA.getSeqLen(mergedId);
    int tLen = seqDbrAA.getSeqLen(targetId);
    
    unsigned int qKey = seqDbrAA.getDbKey(mergedId);
    size_t qCaId = seqDbrCA.getId(qKey);

    unsigned int tKey = seqDbrAA.getDbKey(targetId);
    size_t tCaId = seqDbrCA.getId(tKey);
    
    Coordinate16 qcoords;
    char *qcadata = seqDbrCA.getData(qCaId, 0);
    size_t qCaLength = seqDbrCA.getEntryLen(qCaId);
    float *qCaData = qcoords.read(qcadata, qLen, qCaLength);
    char *merged_aa_seq = seqDbrAA.getData(qCaId, 0);
    
    Coordinate16 tcoords;
    char *tcadata = seqDbrCA.getData(tCaId, 0);
    size_t tCaLength = seqDbrCA.getEntryLen(tCaId);
    float *tCaData = tcoords.read(tcadata, tLen, tCaLength);
    char *target_aa_seq = seqDbrAA.getData(tCaId, 0);

    float TMscore = 0.0;
    TMaligner tmaln(std::max(qLen, tLen)+VECSIZE_FLOAT, 1, 0, false);
    tmaln.initQuery(qCaData, &qCaData[qLen], &qCaData[qLen * 2], merged_aa_seq, qLen);
    Matcher::result_t res = tmaln.align(targetId, tCaData, &tCaData[tLen], &tCaData[tLen * 2], target_aa_seq, tLen, TMscore);
    res.backtrace = Matcher::uncompressAlignment(res.backtrace);
    res.score /= 100;

    return res;
}

// copy from one to two
void copyInstructions(std::vector<Instruction> &one, std::vector<Instruction> &two) {
    for (Instruction ins : one) {
        two.emplace_back(static_cast<int>(ins.bits.state), static_cast<int>(ins.bits.count));
    }
}

// copy from one to two
void copyInstructionVectors(std::vector<std::vector<Instruction> > &one, std::vector<std::vector<Instruction> > &two) {
    two.clear();
    two.resize(one.size());
    // for (std::vector<Instruction> vec : one) {
    for (size_t i = 0; i < one.size(); i++) {
        // std::vector<Instruction> tmp;
        copyInstructions(one[i], two[i]);
        // two[i] = tmp;
    }
}

int structuremsa(int argc, const char **argv, const Command& command, bool preCluster) {
    FoldmasonParameters &par = FoldmasonParameters::getFoldmasonInstance();

    // Databases
    const bool touch = (par.preloadMode != Parameters::PRELOAD_MODE_MMAP);
    par.parseParameters(argc, argv, command, true, 0, MMseqsParameter::COMMAND_ALIGN);

    DBReader<unsigned int> seqDbrAA(par.db1.c_str(), par.db1Index.c_str(), par.threads, DBReader<unsigned int>::USE_INDEX|DBReader<unsigned int>::USE_DATA|DBReader<unsigned int>::USE_LOOKUP_REV);
    seqDbrAA.open(DBReader<unsigned int>::NOSORT);
    DBReader<unsigned int> seqDbr3Di((par.db1+"_ss").c_str(), (par.db1+"_ss.index").c_str(), par.threads, DBReader<unsigned int>::USE_INDEX|DBReader<unsigned int>::USE_DATA);
    seqDbr3Di.open(DBReader<unsigned int>::NOSORT);
    DBReader<unsigned int> seqDbrCA((par.db1+"_ca").c_str(), (par.db1+"_ca.index").c_str(), par.threads, DBReader<unsigned int>::USE_INDEX|DBReader<unsigned int>::USE_DATA);
    seqDbrCA.open(DBReader<unsigned int>::NOSORT);

    IndexReader qdbrH(par.db1, par.threads, IndexReader::HEADERS, touch ? IndexReader::PRELOAD_INDEX : 0);
    
    Debug(Debug::INFO) << "Got databases\n";

    SubstitutionMatrix subMat_3di(par.scoringMatrixFile.values.aminoacid().c_str(), par.bitFactor3Di, par.scoreBias3di);
    std::string blosum;
    for (size_t i = 0; i < par.substitutionMatrices.size(); i++) {
        if (par.substitutionMatrices[i].name == "blosum62.out") {
            std::string matrixData((const char *)par.substitutionMatrices[i].subMatData, par.substitutionMatrices[i].subMatDataLen);
            std::string matrixName = par.substitutionMatrices[i].name;
            char * serializedMatrix = BaseMatrix::serialize(matrixName, matrixData);
            blosum.assign(serializedMatrix);
            free(serializedMatrix);
        }
    }
    SubstitutionMatrix subMat_aa(blosum.c_str(), par.bitFactorAa, par.scoreBiasAa);

    Debug(Debug::INFO) << "Got substitution matrices\n";

    // Initialise MSAs, Sequence objects
    size_t sequenceCnt = seqDbrAA.getSize();
    
    // Current representation of sequences
    std::vector<std::vector<Instruction> > cigars_aa(sequenceCnt);
    std::vector<std::vector<Instruction> > cigars_ss(sequenceCnt);
    
    // Current clusters of structures (indices)
    std::vector<std::vector<size_t> > groups(sequenceCnt);

    // map i <=> dbKey. used in LDDT calculation to retrieve CA
    std::vector<size_t> dbKeys(sequenceCnt);

    std::vector<std::string> mappings(sequenceCnt);
    std::vector<size_t> idMappings(sequenceCnt);
    
    // std::map<std::string, int> seqLens;
    std::vector<int> seqLens(sequenceCnt);

    // int maxSeqLength = 0;
    int maxSeqLength = par.maxSeqLen;

    // TODO: could parallelise this, just need to have reduction for maxSeqLength
    for (size_t i = 0; i < sequenceCnt; i++) {
        unsigned int seqKeyAA = seqDbrAA.getDbKey(i);
        unsigned int seqKey3Di = seqDbr3Di.getDbKey(i);
        size_t seqIdAA = seqDbrAA.getId(seqKeyAA);
        size_t seqId3Di = seqDbr3Di.getId(seqKey3Di);

        dbKeys[i] = seqKeyAA;

        size_t length = seqDbrAA.getSeqLen(seqIdAA);
        const char* seq_aa = seqDbrAA.getData(seqIdAA, 0);
        const char* seq_ss = seqDbr3Di.getData(seqId3Di, 0);

        groups[i].push_back(i);
        for (size_t j = 0; j < length; j++) {
            // Default state is SEQ (no gaps yet)
            cigars_aa[i].emplace_back(seq_aa[j]);
            cigars_ss[i].emplace_back(seq_ss[j]);
        }

        maxSeqLength = std::max(maxSeqLength, static_cast<int>(length));
        mappings[i]  = std::string(length, '0');

        // Map each sequence id to itself for now
        idMappings[i] = i;
        seqLens[i] = length;
    }
   
    // TODO: dynamically calculate and re-init PSSMCalculator/MsaFilter each iteration
    Debug(Debug::INFO) << "Initialised MSAs, Sequence objects\n";

    // Substitution matrices needed for query profile
    int8_t *tinySubMatAA  = (int8_t*) mem_align(ALIGN_INT, subMat_aa.alphabetSize * 32);
    int8_t *tinySubMat3Di = (int8_t*) mem_align(ALIGN_INT, subMat_3di.alphabetSize * 32);

    for (int i = 0; i < subMat_3di.alphabetSize; i++)
        for (int j = 0; j < subMat_3di.alphabetSize; j++)
            tinySubMat3Di[i * subMat_3di.alphabetSize + j] = subMat_3di.subMatrix[i][j]; // for farrar profile
    for (int i = 0; i < subMat_aa.alphabetSize; i++)
        for (int j = 0; j < subMat_aa.alphabetSize; j++)
            tinySubMatAA[i * subMat_aa.alphabetSize + j] = subMat_aa.subMatrix[i][j];

    Debug(Debug::INFO) << "Set up tiny substitution matrices\n";

    bool * alreadyMerged = new bool[sequenceCnt];
   
    DBReader<unsigned int> * cluDbr = NULL;

    if (preCluster) {
        // consider everything merged and unmerge the ones that are not
        memset(alreadyMerged, 1, sizeof(bool) * sequenceCnt);
        cluDbr = new DBReader<unsigned int>(
            par.db2.c_str(),
            par.db2Index.c_str(),
            par.threads,
            DBReader<unsigned int>::USE_INDEX | DBReader<unsigned int>::USE_DATA
        );
        cluDbr->open(DBReader<unsigned int>::LINEAR_ACCCESS);
        // mark all sequences that are already clustered as merged
        for(size_t i = 0; i < cluDbr->getSize(); i++){
            unsigned int dbKey = cluDbr->getDbKey(i);
            alreadyMerged[dbKey] = 0;
        }
    } else {
        memset(alreadyMerged, 0, sizeof(bool) * sequenceCnt);
    }       
    
    // Check if guide tree argument given
    // Try parse --> read if non-empty, otherwise generate one and write
    std::string tree;
    std::vector<AlnSimple> hits;
    std::vector<size_t> merges;

    if (par.guideTree != "") {
        std::string line;
        std::ifstream newick(par.guideTree);
        if (newick.is_open()) {
            while (std::getline(newick, line))
                tree += line;
            newick.close();
        }
    }

    if (tree != "") {
        Debug(Debug::INFO) << "Parsing tree: " << tree << '\n';
        NewickParser::Node* root = NewickParser::parse(tree);
        // std::string nw = NewickParser::toNewick(root);
        // assert(nw == tree);
        
        std::vector<std::string> linkage;
        NewickParser::postOrder(root, &linkage);
        delete root;

        for (size_t i = 0; i < linkage.size(); i += 2) {
            AlnSimple hit;
            
            size_t queryLookupId = seqDbrAA.getLookupIdByAccession(linkage[i]);
            if (queryLookupId == SIZE_MAX) {
                Debug(Debug::ERROR) << "Could not find name " << linkage[i] << " in lookup\n";
                exit(1);
            }
            unsigned int queryKey = seqDbrAA.getLookupKey(queryLookupId);
            size_t queryId = seqDbrAA.getId(queryKey);
            hit.queryId = queryId;
            
            size_t targetLookupId = seqDbrAA.getLookupIdByAccession(linkage[i + 1]);
            if (targetLookupId == SIZE_MAX) {
                Debug(Debug::ERROR) << "Could not find name " << linkage[i + 1] << " in lookup\n";
                exit(1);
            }
            
            unsigned int targetKey = seqDbrAA.getLookupKey(targetLookupId);
            size_t targetId = seqDbrAA.getId(targetKey);
            hit.targetId = targetId;
            
            if (queryId == targetId) {
                continue;
            }

            hit.score = 0;
            hits.push_back(hit);
        }
        
        Debug(Debug::INFO) << "Optimising merge order\n";
        hits = reorderLinkage(hits, merges, sequenceCnt);
    } else {
        hits = updateAllScores(
            seqDbrAA,
            seqDbr3Di,
            tinySubMatAA,
            tinySubMat3Di,
            &subMat_aa,
            &subMat_3di,
            alreadyMerged,
            par.maxSeqLen,
            subMat_3di.alphabetSize,
            par.compBiasCorrection,
            par.compBiasCorrectionScale
        );
        if (cluDbr != NULL) {
            // add external hits to the list
            std::vector<AlnSimple> externalHits = parseAndScoreExternalHits(
                seqDbrAA,
                seqDbr3Di,
                cluDbr,
                tinySubMatAA,
                tinySubMat3Di,
                &subMat_aa,
                &subMat_3di,
                par.maxSeqLen,
                subMat_3di.alphabetSize,
                par.compBiasCorrection,
                par.compBiasCorrectionScale
            );
            // maybe a bit dangerous because memory of hits might be doubled
            for (size_t i = 0; i < externalHits.size(); i++)
                hits.push_back(externalHits[i]);
        }
        Debug(Debug::INFO) << "Performing initial all vs all alignments\n";
        sortHitsByScore(hits);
        
        Debug(Debug::INFO) << "Generating guide tree\n";
        hits = mst(hits, sequenceCnt);

        Debug(Debug::INFO) << "Optimising merge order\n";
        hits = reorderLinkage(hits, merges, sequenceCnt);

        NewickParser::Node* root = NewickParser::buildTree(hits); 
        NewickParser::addNames(root, &qdbrH);
        std::string nw = NewickParser::toNewick(root);
        std::string treeFile = par.filenames[par.filenames.size()-1] + ".nw";
        Debug(Debug::INFO) << "Writing guide tree to: " << treeFile << '\n';
        std::ofstream guideTree(treeFile, std::ofstream::out);
        guideTree << nw;
        guideTree.close();
        delete root;
    }
   
    if (par.verbosity > Debug::INFO) {
        int idx = 0;
        size_t qHeaderId, tHeaderId;
        unsigned int qKey, tKey;
        std::string qHeader, tHeader;
        for (size_t i = 0; i < merges.size(); i++) {
            Debug(Debug::INFO) << "Merging " << merges[i] << " sequences\n";
            for (size_t j = 0; j < merges[i]; j++) {
                qKey = seqDbrAA.getDbKey(hits[idx + j].queryId);
                qHeaderId = qdbrH.sequenceReader->getId(qKey);
                qHeader = Util::parseFastaHeader(qdbrH.sequenceReader->getData(qHeaderId, 0));
                tKey = seqDbrAA.getDbKey(hits[idx + j].targetId);
                tHeaderId = qdbrH.sequenceReader->getId(tKey);
                tHeader = Util::parseFastaHeader(qdbrH.sequenceReader->getData(tHeaderId, 0));
                Debug(Debug::INFO) << "  " << qHeader << "\t" << tHeader << '\t' << hits[idx + j].score << '\n';
            }
            idx += merges[i];
        }
    }

    size_t finalMSAId = 0;
    
    // FIXME this has to be outside OMP block I think?
    // Store profile strings for each merged sequence using merged db key
    std::unordered_map<unsigned int, std::pair<std::string, std::string>> profiles;

    Debug(Debug::INFO) << "Begin progressive alignment\n";

#pragma omp parallel
{
    unsigned int thread_idx = 0;
#ifdef OPENMP
    thread_idx = static_cast<unsigned int>(omp_get_thread_num());
#endif

    // Initialise alignment objects per thread
    StructureSmithWaterman structureSmithWaterman(par.maxSeqLen, subMat_3di.alphabetSize, par.compBiasCorrection, par.compBiasCorrectionScale, &subMat_aa, &subMat_3di);
    MsaFilter filter_aa(maxSeqLength + 1, sequenceCnt + 1, &subMat_aa, par.gapOpen.values.aminoacid(), par.gapExtend.values.aminoacid());
    MsaFilter filter_3di(maxSeqLength + 1, sequenceCnt + 1, &subMat_3di, par.gapOpen.values.aminoacid(), par.gapExtend.values.aminoacid()); 
    PSSMCalculator calculator_aa(&subMat_aa, maxSeqLength + 1, sequenceCnt + 1, par.pcmode, par.pcaAa, par.pcbAa
#ifdef GAP_POS_SCORING
    , par.gapOpen.values.aminoacid(), par.gapPseudoCount
#endif
    );
    PSSMCalculator calculator_3di(&subMat_3di, maxSeqLength + 1, sequenceCnt + 1, par.pcmode, par.pca3di, par.pcb3di
#ifdef GAP_POS_SCORING
    , par.gapOpen.values.aminoacid(), par.gapPseudoCount
#endif
    );

    // Add four seq objects per alignee per thread
    // Amino acid profile/sequence, 3Di profile/sequence
    Sequence seqMergedAaAa(par.maxSeqLen, Parameters::DBTYPE_AMINO_ACIDS, (const BaseMatrix *) &subMat_aa,  0, false, par.compBiasCorrection);
    Sequence seqMergedAaPr(par.maxSeqLen, Parameters::DBTYPE_HMM_PROFILE, (const BaseMatrix *) &subMat_aa,  0, false, par.compBiasCorrection);
    Sequence seqMergedSsAa(par.maxSeqLen, Parameters::DBTYPE_AMINO_ACIDS, (const BaseMatrix *) &subMat_3di, 0, false, par.compBiasCorrection);
    Sequence seqMergedSsPr(par.maxSeqLen, Parameters::DBTYPE_HMM_PROFILE, (const BaseMatrix *) &subMat_3di, 0, false, par.compBiasCorrection);
    Sequence seqTargetAaAa(par.maxSeqLen, Parameters::DBTYPE_AMINO_ACIDS, (const BaseMatrix *) &subMat_aa,  0, false, par.compBiasCorrection);
    Sequence seqTargetAaPr(par.maxSeqLen, Parameters::DBTYPE_HMM_PROFILE, (const BaseMatrix *) &subMat_aa,  0, false, par.compBiasCorrection);
    Sequence seqTargetSsAa(par.maxSeqLen, Parameters::DBTYPE_AMINO_ACIDS, (const BaseMatrix *) &subMat_3di, 0, false, par.compBiasCorrection);
    Sequence seqTargetSsPr(par.maxSeqLen, Parameters::DBTYPE_HMM_PROFILE, (const BaseMatrix *) &subMat_3di, 0, false, par.compBiasCorrection);
    
    // Pointers to whichever Sequence we need to use per-alignment
    Sequence *seqMergedAa;
    Sequence *seqMergedSs;
    Sequence *seqTargetAa;
    Sequence *seqTargetSs;
    
    int index = 0; // in hit list
    for (size_t i = 0; i < merges.size(); i++) {

#pragma omp for schedule(dynamic, 1)
        for (size_t j = 0; j < merges[i]; j++) {
            size_t mergedId = std::min(hits[index + j].queryId, hits[index + j].targetId);
            size_t targetId = std::max(hits[index + j].queryId, hits[index + j].targetId);
            assert(mergedId != targetId);

            mergedId = idMappings[mergedId];
            targetId = idMappings[targetId];
            bool queryIsProfile = false;
            bool targetIsProfile = false;
            
            // If there is an existing profile, use the Sequence::HMM_PROFILE type Sequence
            auto profile = profiles.find(mergedId);
            size_t key = seqDbrAA.getDbKey(mergedId);
            size_t id  = seqDbrAA.getId(key);
            if (profile != profiles.end()) {
                seqMergedAaPr.mapSequence(id, key, profile->second.first.c_str(),  profile->second.first.length() / Sequence::PROFILE_READIN_SIZE);
                seqMergedSsPr.mapSequence(id, key, profile->second.second.c_str(), profile->second.second.length() / Sequence::PROFILE_READIN_SIZE);
                seqMergedAa = &seqMergedAaPr;
                seqMergedSs = &seqMergedSsPr;
                queryIsProfile = true;
            } else {
                seqMergedAaAa.mapSequence(id, key, seqDbrAA.getData(id, thread_idx), seqDbrAA.getSeqLen(id));
                seqMergedSsAa.mapSequence(id, key, seqDbr3Di.getData(id, thread_idx), seqDbr3Di.getSeqLen(id));
                seqMergedAa = &seqMergedAaAa;
                seqMergedSs = &seqMergedSsAa;
            }
            profile = profiles.find(targetId);
            key = seqDbrAA.getDbKey(targetId);
            id  = seqDbrAA.getId(key);
            if (profile != profiles.end()) {
                seqTargetAaPr.mapSequence(id, key, profile->second.first.c_str(),  profile->second.first.length() / Sequence::PROFILE_READIN_SIZE);
                seqTargetSsPr.mapSequence(id, key, profile->second.second.c_str(), profile->second.second.length() / Sequence::PROFILE_READIN_SIZE);
                seqTargetAa = &seqTargetAaPr;
                seqTargetSs = &seqTargetSsPr;
                targetIsProfile = true;
            } else {
                seqTargetAaAa.mapSequence(id, key, seqDbrAA.getData(id, thread_idx), seqDbrAA.getSeqLen(id));
                seqTargetSsAa.mapSequence(id, key, seqDbr3Di.getData(id, thread_idx), seqDbr3Di.getSeqLen(id));
                seqTargetAa = &seqTargetAaAa;
                seqTargetSs = &seqTargetSsAa;
            }

            // Always merge onto sequence with most information
            if (targetIsProfile && !queryIsProfile) {
                std::swap(mergedId, targetId);
                std::swap(seqMergedAa, seqTargetAa);
                std::swap(seqMergedSs, seqTargetSs);
                std::swap(queryIsProfile, targetIsProfile);
            } else if (targetIsProfile && queryIsProfile) {
                float q_neff_sum = 0.0;
                float t_neff_sum = 0.0;
                for (int i = 0; i < seqMergedSs->L; i++) {
                    q_neff_sum += seqMergedSs->neffM[i];
                }
                for (int i = 0; i < seqTargetSs->L; i++) {
                    t_neff_sum += seqTargetSs->neffM[i];
                }
                if (q_neff_sum <= t_neff_sum) {
                    std::swap(mergedId, targetId);
                    std::swap(seqMergedAa, seqTargetAa);
                    std::swap(seqMergedSs, seqTargetSs);
                    std::swap(queryIsProfile, targetIsProfile);
                }
            }

            // Make sure all relevant ids are updated
            for (size_t k = 0; k < sequenceCnt; k++) {
                if (idMappings[k] == targetId) {
                    idMappings[k] = mergedId;
                }
            }

            // Convert 010101 mask to [ 0, 2, 4 ] index mapping
            std::vector<int> map1 = maskToMapping(mappings[mergedId]);
            std::vector<int> map2 = maskToMapping(mappings[targetId]);
            structureSmithWaterman.ssw_init(
                seqMergedAa,
                seqMergedSs,
                tinySubMatAA,
                tinySubMat3Di,
                &subMat_aa
            );
            Matcher::result_t res = pairwiseAlignment(
                structureSmithWaterman,
                seqMergedAa->L,
                seqMergedAa,
                seqMergedSs, 
                seqTargetAa,
                seqTargetSs,
                par.gapOpen.values.aminoacid(),
                par.gapExtend.values.aminoacid(),
                &subMat_aa,
                &subMat_3di,
                par.compBiasCorrection
            );
            std::vector<Instruction> qBt;
            std::vector<Instruction> tBt;
            getMergeInstructions(res, map1, map2, qBt, tBt);
        
            // If neither are profiles, do TM-align as well and take the best alignment
            bool tmaligned = false;
            // if (false) {
            if (!queryIsProfile && !targetIsProfile) {
                Matcher::result_t tmRes = pairwiseTMAlign(mergedId, targetId, seqDbrAA, seqDbrCA);
                std::vector<Instruction> qBtTM;
                std::vector<Instruction> tBtTM;
                getMergeInstructions(tmRes, map1, map2, qBtTM, tBtTM);

                // TODO put this info in struct?
                int qPreSequence = map1[tmRes.qStartPos];
                int qPreGaps     = map2[tmRes.dbStartPos];
                int qEndSequence = map1[map1.size() - 1] - map1.at(tmRes.qEndPos);
                int qEndGaps     = map2[map2.size() - 1] - map2.at(tmRes.dbEndPos);
                int tPreSequence = qPreGaps;
                int tPreGaps     = qPreSequence;
                int tEndSequence = qEndGaps;
                int tEndGaps     = qEndSequence;
                std::vector<Instruction> query_aa;
                std::vector<Instruction> query_ss;
                std::vector<Instruction> target_aa;
                std::vector<Instruction> target_ss;
                copyInstructions(cigars_aa[mergedId], query_aa);
                copyInstructions(cigars_ss[mergedId], query_ss);
                copyInstructions(cigars_aa[targetId], target_aa);
                copyInstructions(cigars_ss[targetId], target_ss);
                updateQueryCIGAR(query_aa, query_ss, qBtTM, qPreGaps, qPreSequence, qEndGaps, qEndSequence);
                updateTargetCIGAR(target_aa, target_ss, tBtTM, tPreGaps, tPreSequence, tEndGaps, tEndSequence);
                
                // std::cout << "TM Alignment:\n";
                // std::cout << expand(query_aa) << '\n';
                // std::cout << expand(target_aa) << '\n';
               
                // mock vectors for lddt
                // TODO make alternative LDDT calculation fn so we don't have to do this
                std::vector<std::vector<Instruction> > cigars_tm = { query_aa, target_aa };
                std::vector<size_t> subset_tm = { 0, 1 };
                std::vector<size_t> indices_tm = { dbKeys[mergedId], dbKeys[targetId] };
                std::vector<int>    lengths_tm = { seqLens[mergedId], seqLens[targetId] };

                float lddtTM = std::get<2>(calculate_lddt(cigars_tm, subset_tm, indices_tm, lengths_tm, &seqDbrCA, par.pairThreshold));
                // std::cout << "got TM lddt: " << lddtTM << '\n';
                
                // adjust cigars with 3Di alignment result
                qPreSequence = map1[res.qStartPos];
                qPreGaps     = map2[res.dbStartPos];
                qEndSequence = map1[map1.size() - 1] - map1.at(res.qEndPos);
                qEndGaps     = map2[map2.size() - 1] - map2.at(res.dbEndPos);
                tPreSequence = qPreGaps;
                tPreGaps     = qPreSequence;
                tEndSequence = qEndGaps;
                tEndGaps     = qEndSequence;
                query_aa.clear();
                query_ss.clear();
                target_aa.clear();
                target_ss.clear();
                copyInstructions(cigars_aa[mergedId], query_aa);
                copyInstructions(cigars_ss[mergedId], query_ss);
                copyInstructions(cigars_aa[targetId], target_aa);
                copyInstructions(cigars_ss[targetId], target_ss);
                updateQueryCIGAR(query_aa, query_ss, qBt, qPreGaps, qPreSequence, qEndGaps, qEndSequence);
                updateTargetCIGAR(target_aa, target_ss, tBt, tPreGaps, tPreSequence, tEndGaps, tEndSequence);
                cigars_tm[0] = query_aa;
                cigars_tm[1] = target_aa;
                
                // std::cout << "3Di Alignment:\n";
                // std::cout << expand(query_aa) << '\n';
                // std::cout << expand(target_aa) << '\n';

                float lddt3Di = std::get<2>(calculate_lddt(cigars_tm, subset_tm, indices_tm, lengths_tm, &seqDbrCA, par.pairThreshold));
                // std::cout << "got 3Di lddt: " << lddt3Di << '\n';

                if (lddtTM > lddt3Di) {
                    res = tmRes;
                    qBt = qBtTM;
                    tBt = tBtTM;
                    tmaligned = true;
                }
            }
            updateCIGARS(groups[mergedId], groups[targetId], cigars_aa, cigars_ss, res, map1, map2, qBt, tBt);           
            groups[mergedId].insert(groups[mergedId].end(), groups[targetId].begin(), groups[targetId].end());
            groups[targetId].clear();
            mappings[targetId].clear();

            // testSeqLens(groups[mergedId], cigars_aa, seqLens);
            
            if (par.verbosity > Debug::INFO) {
                unsigned int qKey = seqDbrAA.getDbKey(mergedId);
                size_t qHeaderId = qdbrH.sequenceReader->getId(qKey);
                unsigned int tKey = seqDbrAA.getDbKey(targetId);
                size_t tHeaderId = qdbrH.sequenceReader->getId(tKey);
                Debug(Debug::INFO)
                    << std::to_string(queryIsProfile) << "\t"
                    << std::to_string(targetIsProfile) << "\t"
                    << Util::parseFastaHeader(qdbrH.sequenceReader->getData(qHeaderId, 0))
                    << "\t"
                    << Util::parseFastaHeader(qdbrH.sequenceReader->getData(tHeaderId, 0))
                    << "\t"
                    << SSTR(res.score);
                if (tmaligned){
                    Debug(Debug::INFO) << "\t(TM-align)";
                }
                Debug(Debug::INFO) << "\n";
            }

            mappings[mergedId] = computeProfileMask(
                groups[mergedId],
                cigars_aa,
                seqLens,
                subMat_aa,
                par.matchRatio
            );
            std::string profile_aa = msa2profile(
                groups[mergedId],
                cigars_aa,
                mappings[mergedId],
                calculator_aa,
                filter_aa,
                subMat_aa,
                par.filterMsa,
                par.compBiasCorrection,
                par.qid,
                par.filterMaxSeqId,
                par.Ndiff,
                par.covMSAThr,
                par.qsc,
                par.filterMinEnable,
                par.wg
            );
            std::string profile_3di = msa2profile(
                groups[mergedId],
                cigars_ss,
                mappings[mergedId],
                calculator_3di,
                filter_3di,
                subMat_3di,
                par.filterMsa,
                par.compBiasCorrection,
                par.qid,
                par.filterMaxSeqId,
                par.Ndiff,
                par.covMSAThr,
                par.qsc,
                par.filterMinEnable,
                par.wg
            );
            assert(profile_aa.length() == profile_3di.length());
            profiles[mergedId] = std::make_pair(profile_aa, profile_3di);
            if (targetIsProfile) {
                profiles.erase(targetId);
            }

            alreadyMerged[targetId] = true;
            finalMSAId = mergedId;
        }
        index += merges[i];
    }

    // Refine alignment -- MUSCLE5 style
    // 1. Partition into two sub-MSAs
    // 2. Remove all-gap columns
    // 3. Create sub-MSA profiles
    // 4. Save profiles -> Sequence objects
    // 5. Pairwise alignment
    // 6. Repeat x100
    // Only run with master thread
#pragma omp master
{
    if (par.refineIters > 0) {
        refineMany(
            tinySubMatAA, tinySubMat3Di, &seqDbrCA, cigars_aa, cigars_ss, calculator_aa,
            filter_aa, subMat_aa, calculator_3di, filter_3di, subMat_3di, structureSmithWaterman,
            par.refineIters, par.compBiasCorrection, par.wg, par.filterMaxSeqId, par.qsc,
            par.Ndiff, par.covMSAThr, par.filterMinEnable, par.filterMsa, par.gapExtend.values.aminoacid(),
            par.gapOpen.values.aminoacid(), par.maxSeqLen, par.qid, par.pairThreshold, dbKeys, seqLens
        );
    }
}

}
    // Write final MSA to file with correct headers
    DBWriter resultWriterAa(
        (par.filenames[par.filenames.size()-1] + "_aa.fa").c_str(),
        (par.filenames[par.filenames.size()-1] + "_aa.index").c_str(),
        static_cast<unsigned int>(par.threads), par.compressed, Parameters::DBTYPE_OMIT_FILE
    );
    DBWriter resultWriter3Di(
        (par.filenames[par.filenames.size()-1] + "_3di.fa").c_str(),
        (par.filenames[par.filenames.size()-1] + "_3di.index").c_str(),
        static_cast<unsigned int>(par.threads), par.compressed, Parameters::DBTYPE_OMIT_FILE
    );
    resultWriterAa.open();
    resultWriterAa.writeStart(0);
    resultWriter3Di.open();
    resultWriter3Di.writeStart(0);
    std::string buffer;
    buffer.reserve(10 * 1024);
    for (size_t i = 0; i < cigars_aa.size(); i++) {
        size_t idx = groups[finalMSAId][i];
        unsigned int key = seqDbrAA.getDbKey(idx);
        size_t headerId = qdbrH.sequenceReader->getId(key);
        std::string header = Util::parseFastaHeader(qdbrH.sequenceReader->getData(headerId, 0));

        buffer.append(1, '>');
        buffer.append(header);
        buffer.append(1, '\n');
        buffer.append(expand(cigars_aa[idx]));
        buffer.append(1, '\n');
        resultWriterAa.writeAdd(buffer.c_str(), buffer.size(), 0);
        buffer.clear();
        
        buffer.append(1, '>');
        buffer.append(header);
        buffer.append(1, '\n');
        buffer.append(expand(cigars_ss[idx]));
        buffer.append(1, '\n');
        resultWriter3Di.writeAdd(buffer.c_str(), buffer.size(), 0);
        buffer.clear();
    } 
    resultWriterAa.writeEnd(0, 0, false, 0);
    resultWriterAa.close(true);
    resultWriter3Di.writeEnd(0, 0, false, 0);
    resultWriter3Di.close(true);
    FileUtil::remove((par.filenames[par.filenames.size()-1] + "_aa.index").c_str());
    FileUtil::remove((par.filenames[par.filenames.size()-1] + "_3di.index").c_str());

    // Cleanup
    delete[] alreadyMerged;
    free(tinySubMatAA);
    free(tinySubMat3Di);
    seqDbrAA.close();
    seqDbr3Di.close();
    seqDbrCA.close();
  
    return EXIT_SUCCESS;
}

int structuremsa(int argc, const char **argv, const Command& command) {
    return structuremsa(argc, argv, command, false);
}

int structuremsacluster(int argc, const char **argv, const Command& command) {
    return structuremsa(argc, argv, command, true);
}
