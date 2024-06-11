#ifndef MSA2LDDT_H
#define MSA2LDDT_H

#include <vector>
#include <iostream>
#include "DBReader.h"
#include "KSeqWrapper.h"
#include "structuremsa.h"
#include <unordered_map>

void parseFasta(
    KSeqWrapper *kseq,
    DBReader<unsigned int> * seqDbrAA,
    DBReader<unsigned int> * seqDbr3Di,
    std::vector<std::string> &headers,
    std::vector<size_t>      &indices,
    std::vector<int>         &lengths,
    std::unordered_map<size_t, std::vector<Instruction> > &cigars_aa,
    std::unordered_map<size_t, std::vector<Instruction> > &cigars_ss,
    int &alnLength
);

std::tuple<std::vector<float>, std::vector<int>, float> calculate_lddt(
    std::unordered_map<size_t, std::vector<Instruction> > &cigars,
    std::vector<size_t> subset,
    std::vector<size_t> &indices,
    std::vector<int> &lengths,
    DBReader<unsigned int> * seqDbrAA,
    DBReader<unsigned int> * seqDbrCA,
    float pairThreshold
);

float getLDDTScore(
    DBReader<unsigned int> &seqDbrAA,
    DBReader<unsigned int> &seqDbr3Di,
    DBReader<unsigned int> &seqDbrCA,
    std::string msa,
    float pairThreshold
);

#endif