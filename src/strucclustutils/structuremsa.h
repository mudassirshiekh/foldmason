#include <iostream>
#include "Matcher.h"
#include "PSSMCalculator.h"
#include "MsaFilter.h"
#include "SubstitutionMatrix.h"
#include "StructureSmithWaterman.h"
#include "Sequence.h"

enum State {
    SEQ = 0,
    GAP = 1
};

struct Instruction {
    bool state;
    int8_t count; // TODO encode as 1 byte [0/1][000000] state/count, union struct lookup
    Instruction(int i_state, int i_count) : state(i_state), count(i_count) {};
    void print() {
        char state_char = (state == SEQ) ? 'S' : 'G';
        std::cout << state_char << " " << count << std::endl;
    }
    char stateChar() { return (state == SEQ) ? 'S' : 'G'; }
};

// Bit field version
// First bit      = match or gap
// Remaining bits = ASCII character or (gap) count
union Instruction2 {
    struct BitFields {
        std::uint8_t state : 1;  // 0 = match, 1 = gap
        std::uint8_t count : 7;  // count < 127
    } bits;
    Instruction2(char c) {
        bits.state = 0;
        bits.count = c;
    }
    Instruction2(int count) {
        bits.state = 1;
        bits.count = count;
    }
    char getCharacter() const {
        return (bits.state == 0) ? static_cast<char>(bits.count) : '-';
    }
};

std::string fastamsa2profile(
    std::string & msa,
    PSSMCalculator &pssmCalculator,
    MsaFilter &filter,
    SubstitutionMatrix &subMat,
    size_t maxSeqLength,
    size_t maxSetSize,
    float matchRatio,
    bool filterMsa,
    bool compBiasCorrection,
    std::string & qid,
    float filterMaxSeqId,
    float Ndiff,
    float covMSAThr,
    float qsc,
    int filterMinEnable,
    bool wg,
    bool *externalMaskedColumns,
    float scoreBias
);

void getMergeInstructions(
    Matcher::result_t &res,
    std::vector<int> &map1,
    std::vector<int> &map2,
    std::vector<Instruction> &qBt,
    std::vector<Instruction> &tBt
);

Matcher::result_t pairwiseAlignment(
    StructureSmithWaterman & aligner,
    unsigned int querySeqLen,
    Sequence *query_aa,
    Sequence *query_3di,
    Sequence *target_aa,
    Sequence *target_3di,
    int gapOpen, int gapExtend,
    SubstitutionMatrix *mat_aa,
    SubstitutionMatrix *mat_3di,
    std::vector<int> &qMap,
    std::vector<int> &tMap
);

std::string mergeTwoMsa(
    std::string &msa1,
    std::string &msa2,
    Matcher::result_t &res,
    std::vector<int> map1,
    std::vector<int> map2,
    std::vector<Instruction> &qBt,
    std::vector<Instruction> &tBt
);

std::vector<int> maskToMapping(std::string mask);