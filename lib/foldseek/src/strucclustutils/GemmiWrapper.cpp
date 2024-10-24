//
// Created by Martin Steinegger on 6/7/21.
//
#include "GemmiWrapper.h"
#include "mmread.hpp"
#ifdef HAVE_ZLIB
#include "gz.hpp"
#endif
#include "input.hpp"
#include "foldcomp.h"
#include "cif.hpp"

#include <algorithm>

GemmiWrapper::GemmiWrapper(){
    threeAA2oneAA = {{"ALA",'A'},  {"ARG",'R'},  {"ASN",'N'}, {"ASP",'D'},
                     {"CYS",'C'},  {"GLN",'Q'},  {"GLU",'E'}, {"GLY",'G'},
                     {"HIS",'H'},  {"ILE",'I'},  {"LEU",'L'}, {"LYS",'K'},
                     {"MET",'M'},  {"PHE",'F'},  {"PRO",'P'}, {"SER",'S'},
                     {"THR",'T'},  {"TRP",'W'},  {"TYR",'Y'}, {"VAL",'V'},
                     // modified res
                     {"MSE",'M'}, {"MLY",'K'}, {"FME",'M'}, {"HYP",'P'},
                     {"TPO",'T'}, {"CSO",'C'}, {"SEP",'S'}, {"M3L",'K'},
                     {"HSK",'H'}, {"SAC",'S'}, {"PCA",'E'}, {"DAL",'A'},
                     {"CME",'C'}, {"CSD",'C'}, {"OCS",'C'}, {"DPR",'P'},
                     {"B3K",'K'}, {"ALY",'K'}, {"YCM",'C'}, {"MLZ",'K'},
                     {"4BF",'Y'}, {"KCX",'K'}, {"B3E",'E'}, {"B3D",'D'},
                     {"HZP",'P'}, {"CSX",'C'}, {"BAL",'A'}, {"HIC",'H'},
                     {"DBZ",'A'}, {"DCY",'C'}, {"DVA",'V'}, {"NLE",'L'},
                     {"SMC",'C'}, {"AGM",'R'}, {"B3A",'A'}, {"DAS",'D'},
                     {"DLY",'K'}, {"DSN",'S'}, {"DTH",'T'}, {"GL3",'G'},
                     {"HY3",'P'}, {"LLP",'K'}, {"MGN",'Q'}, {"MHS",'H'},
                     {"TRQ",'W'}, {"B3Y",'Y'}, {"PHI",'F'}, {"PTR",'Y'},
                     {"TYS",'Y'}, {"IAS",'D'}, {"GPL",'K'}, {"KYN",'W'},
                     {"CSD",'C'}, {"SEC",'C'},
                     // unknown
                     {"UNK",'X'}};
    fixupBuffer = NULL;
}

std::unordered_map<std::string, int> getEntityTaxIDMapping(gemmi::cif::Document& doc) {
    std::unordered_map<std::string, int> entity_to_taxid;
    static const std::vector<std::pair<std::string, std::string>> loops_with_taxids = {
        { "_entity_src_nat.", "?pdbx_ncbi_taxonomy_id"},
        { "_entity_src_gen.", "?pdbx_gene_src_ncbi_taxonomy_id"},
        { "_pdbx_entity_src_syn.", "?ncbi_taxonomy_id"}
    };
    for (gemmi::cif::Block& block : doc.blocks) {
        for (auto&& [loop, taxid] : loops_with_taxids) {
            for (auto row : block.find(loop, {"entity_id", taxid})) {
                if (row.has2(1) == false) {
                    continue;
                }
                std::string entity_id = gemmi::cif::as_string(row[0]);
                if (entity_to_taxid.find(entity_id) != entity_to_taxid.end()) {
                    continue;
                }
                const char* endptr = NULL;
                int taxId = gemmi::no_sign_atoi(row[1].c_str(), &endptr);
                if (endptr != NULL && *endptr == '\0') {
                    entity_to_taxid.emplace(entity_id, taxId);
                }
            }
        }
    }
    return entity_to_taxid;
}

GemmiWrapper::Format mapFormat(gemmi::CoorFormat format) {
    switch (format) {
        case gemmi::CoorFormat::Pdb:
            return GemmiWrapper::Format::Pdb;
        case gemmi::CoorFormat::Mmcif:
            return GemmiWrapper::Format::Mmcif;
        case gemmi::CoorFormat::Mmjson:
            return GemmiWrapper::Format::Mmjson;
        case gemmi::CoorFormat::ChemComp:
            return GemmiWrapper::Format::ChemComp;
        default:
            return GemmiWrapper::Format::Unknown;
    }
}

bool GemmiWrapper::load(const std::string& filename, Format format) {
    if ((format == Format::Foldcomp) || (format == Format::Detect && gemmi::iends_with(filename, ".fcz"))) {
        std::ifstream in(filename, std::ios::binary);
        if (!in) {
            return false;
        }
        return loadFoldcompStructure(in, filename);
    }
    try {
#ifdef HAVE_ZLIB
        gemmi::MaybeGzipped infile(filename);
#else
        gemmi::BasicInput infile(filename);
#endif
        if (format == Format::Detect) {
            format = mapFormat(gemmi::coor_format_from_ext(infile.basepath()));
        }
        gemmi::Structure st;
        std::unordered_map<std::string, int> entity_to_tax_id;
        switch (format) {
            case Format::Mmcif: {
                gemmi::CharArray mem = read_into_buffer(infile);
                char* data = mem.data();
                size_t dataSize = mem.size();

                // hack to fix broken _citation.title in AF3
                const char target0[] = "Accurate structure prediction of biomolecular interactions with AlphaFold 3\n";
                size_t target0Len = sizeof(target0) - 1;
                const char target1[] = "_citation.title";
                size_t target1Len = sizeof(target1) - 1;
                char* it = std::search(data, data + dataSize, target0, target0 + target0Len);
                if (it != data + dataSize) {
                    while (it > data && *(it - 1) != '\n') {
                        it--;
                    }
                    if (strncmp(it, target1, target1Len) == 0) {
                        it[0] = '#';
                        it[1] = ' ';
                    }
                }

                gemmi::cif::Document doc = gemmi::cif::read_memory(mem.data(), mem.size(), infile.path().c_str());
                entity_to_tax_id = getEntityTaxIDMapping(doc);
                st = gemmi::make_structure(doc);
                break;
            }
            case Format::Mmjson: {
                gemmi::cif::Document doc = gemmi::cif::read_mmjson(infile);
                entity_to_tax_id = getEntityTaxIDMapping(doc);
                st = gemmi::make_structure(doc);
                break;
            }
            case Format::ChemComp: {
                gemmi::cif::Document doc = gemmi::cif::read(infile);
                entity_to_tax_id = getEntityTaxIDMapping(doc);
                st = gemmi::make_structure_from_chemcomp_doc(doc);
                break;
            }
            default:
                st = gemmi::read_pdb(infile);
        }
        updateStructure((void*) &st, filename, entity_to_tax_id);
    } catch (...) {
        return false;
    }
    return true;
}

// https://stackoverflow.com/questions/1448467/initializing-a-c-stdistringstream-from-an-in-memory-buffer/1449527
struct OneShotReadBuf : public std::streambuf
{
    OneShotReadBuf(char* s, std::size_t n)
    {
        setg(s, s, s + n);
    }
};

bool GemmiWrapper::loadFromBuffer(const char * buffer, size_t bufferSize, const std::string& name, GemmiWrapper::Format format) {
    if ((format == Format::Foldcomp) || (format == Format::Detect && (bufferSize > MAGICNUMBER_LENGTH && strncmp(buffer, MAGICNUMBER, MAGICNUMBER_LENGTH) == 0))) {
        OneShotReadBuf buf((char *) buffer, bufferSize);
        std::istream istr(&buf);
        if (!istr) {
            return false;
        }
        return loadFoldcompStructure(istr, name);
    }
    try {
#ifdef HAVE_ZLIB
        gemmi::MaybeGzipped infile(name);
#else
        gemmi::BasicInput infile(name);
#endif
        if (format == Format::Detect) {
            format = mapFormat(gemmi::coor_format_from_ext(infile.basepath()));
        }

        gemmi::Structure st;
        std::unordered_map<std::string, int> entity_to_tax_id;
        switch (format) {
            case Format::Pdb:
                st = gemmi::pdb_impl::read_pdb_from_stream(gemmi::MemoryStream(buffer, bufferSize), name, gemmi::PdbReadOptions());
                break;
            case Format::Mmcif: {
                const char* targetBuffer = buffer;
                // hack to fix broken _citation.title in AF3
                const char target0[] = "Accurate structure prediction of biomolecular interactions with AlphaFold 3\n";
                size_t target0Len = sizeof(target0) - 1;
                const char target1[] = "_citation.title";
                size_t target1Len = sizeof(target1) - 1;
                const char* it = std::search(targetBuffer, targetBuffer + bufferSize, target0, target0 + target1Len);
                if (it != targetBuffer + bufferSize) {
                    if (fixupBuffer == NULL) {
                        fixupBufferSize = bufferSize;
                        fixupBuffer = (char*)malloc(fixupBufferSize);
                    } else if (bufferSize > fixupBufferSize) {
                        fixupBufferSize = bufferSize * 1.5;
                        fixupBuffer = (char*)realloc(fixupBuffer, fixupBufferSize);
                    }
                    memcpy(fixupBuffer, targetBuffer, bufferSize);
                    while (it > targetBuffer && *(it - 1) != '\n') {
                        it--;
                    }
                    if (strncmp(it, target1, target1Len) == 0) {
                        *(fixupBuffer + (it - targetBuffer)) = '#';
                        *(fixupBuffer + (it - targetBuffer) + 1) = ' ';
                    }
                    targetBuffer = fixupBuffer;
                }
                gemmi::cif::Document doc = gemmi::cif::read_memory(targetBuffer, bufferSize, name.c_str());
                entity_to_tax_id = getEntityTaxIDMapping(doc);
                st = gemmi::make_structure(doc);
                break;
            }
            case Format::Mmjson: {
                char* bufferCopy = (char*)malloc(bufferSize + 1 * sizeof(char));
                if (bufferCopy == NULL) {
                    return false;
                }
                if (memcpy(bufferCopy, buffer, bufferSize) == NULL) {
                    free(bufferCopy);
                    return false;
                }
                bufferCopy[bufferSize] = '\0';
                gemmi::cif::Document doc = gemmi::cif::read_mmjson_insitu(bufferCopy, bufferSize, name);
                entity_to_tax_id = getEntityTaxIDMapping(doc);
                st = gemmi::make_structure(doc);
                free(bufferCopy);
                break;
            }
            case Format::ChemComp: {
                gemmi::cif::Document doc = gemmi::cif::read_memory(buffer, bufferSize, name.c_str());
                entity_to_tax_id = getEntityTaxIDMapping(doc);
                st = gemmi::make_structure_from_chemcomp_doc(doc);
                break;
            }
            default:
                return false;
        }
        updateStructure((void*) &st, name, entity_to_tax_id);
    } catch (...) {
        return false;
    }
    return true;
}

bool GemmiWrapper::loadFoldcompStructure(std::istream& stream, const std::string& filename) {
    std::cout.setstate(std::ios_base::failbit);
    Foldcomp fc;
    int res = fc.read(stream);
    if (res != 0) {
        return false;
    }
    std::vector<AtomCoordinate> coordinates;
    fc.useAltAtomOrder = false;
    res = fc.decompress(coordinates);
    if (res != 0) {
        return false;
    }
    std::cout.clear();
    if (coordinates.size() == 0) {
        return false;
    }
    seq3di.clear();
    taxIds.clear();
    title.clear();
    chain.clear();
    names.clear();
    chainNames.clear();
    modelIndices.clear();
    ca.clear();
    ca_bfactor.clear();
    c.clear();
    cb.clear();
    n.clear();
    ami.clear();
    title.append(fc.strTitle);
    names.push_back(filename);
    const AtomCoordinate& first = coordinates[0];
    chainNames.push_back(first.chain);
    modelCount = 1;
    modelIndices.push_back(modelCount);
    int residueIndex = INT_MAX;
    Vec3 ca_atom = {NAN, NAN, NAN};
    Vec3 cb_atom = {NAN, NAN, NAN};
    Vec3 n_atom  = {NAN, NAN, NAN};
    Vec3 c_atom  = {NAN, NAN, NAN};
    float ca_atom_bfactor = 0.0;
    for (std::vector<AtomCoordinate>::const_iterator it = coordinates.begin(); it != coordinates.end(); ++it) {
        const AtomCoordinate& atom = *it;
        if (atom.residue_index != residueIndex) {
            if (residueIndex != INT_MAX) {
                ca.push_back(ca_atom);
                cb.push_back(cb_atom);
                n.push_back(n_atom);
                c.push_back(c_atom);
                ca_bfactor.push_back(ca_atom_bfactor);
                ca_atom = {NAN, NAN, NAN};
                cb_atom = {NAN, NAN, NAN};
                n_atom  = {NAN, NAN, NAN};
                c_atom  = {NAN, NAN, NAN};
                ca_atom_bfactor = 0.0;
            }
            if (threeAA2oneAA.find(atom.residue) == threeAA2oneAA.end()) {
                ami.push_back('X');
            } else {
                ami.push_back(threeAA2oneAA[atom.residue]);
            }
            residueIndex = atom.residue_index;
        }

        if (atom.atom == "CA") {
            ca_atom = { atom.coordinate.x, atom.coordinate.y, atom.coordinate.z };
            ca_atom_bfactor = atom.tempFactor;
        } else if (atom.atom == "CB") {
            cb_atom = { atom.coordinate.x, atom.coordinate.y, atom.coordinate.z };
        } else if (atom.atom == "N") {
            n_atom = { atom.coordinate.x, atom.coordinate.y, atom.coordinate.z };
        } else if (atom.atom == "C") {
            c_atom = { atom.coordinate.x, atom.coordinate.y, atom.coordinate.z };
        }
    }
    ca.push_back(ca_atom);
    cb.push_back(cb_atom);
    n.push_back(n_atom);
    c.push_back(c_atom);
    ca_bfactor.push_back(ca_atom_bfactor);
    chain.emplace_back(0, ca.size());
    return true;
}

void GemmiWrapper::updateStructure(void * void_st, const std::string& filename, std::unordered_map<std::string, int>& entity_to_tax_id) {
    gemmi::Structure * st = (gemmi::Structure *) void_st;

    title.clear();
    chain.clear();
    names.clear();
    chainNames.clear();
    modelIndices.clear();
    modelCount = 0;
    ca.clear();
    ca_bfactor.clear();
    c.clear();
    cb.clear();
    n.clear();
    ami.clear();
    taxIds.clear();
    title.append(st->get_info("_struct.title"));
    size_t currPos = 0;
    for (gemmi::Model& model : st->models){
        modelCount++;
        for (gemmi::Chain& ch : model.chains) {
            size_t chainStartPos = currPos;
            size_t pos = filename.find_last_of("\\/");
            std::string name = (std::string::npos == pos)
                               ? filename
                               : filename.substr(pos+1, filename.length());
            //name.push_back('_');
            chainNames.push_back(ch.name);
            char* rest;
            errno = 0;
            unsigned int modelNumber = strtoul(model.name.c_str(), &rest, 10);
            if ((rest != model.name.c_str() && *rest != '\0') || errno == ERANGE) {
                modelIndices.push_back(modelCount);
            }else{
                modelIndices.push_back(modelNumber);
            }

            names.push_back(name);
            int taxId = -1;
            for (gemmi::Residue &res : ch.first_conformer()) {
                if (taxId == -1) {
                    auto it = entity_to_tax_id.find(res.entity_id);
                    if (it != entity_to_tax_id.end()) {
                        taxId = it->second;
                    }
                }
                bool isHetAtomInList = res.het_flag == 'H' && threeAA2oneAA.find(res.name) != threeAA2oneAA.end();
                if (isHetAtomInList == false && res.het_flag != 'A')
                    continue;
                if (isHetAtomInList) {
                    bool notPolymer = res.entity_type != gemmi::EntityType::Polymer;
                    if (notPolymer == true) {
                        continue;
                    }
                    bool hasCA = false;
                    for (gemmi::Atom &atom : res.atoms) {
                        if (atom.name == "CA") {
                            hasCA = true;
                            break;
                        }
                    }
                    if (hasCA == false) {
                        continue;
                    }
                }
                Vec3 ca_atom = {NAN, NAN, NAN};
                Vec3 cb_atom = {NAN, NAN, NAN};
                Vec3 n_atom  = {NAN, NAN, NAN};
                Vec3 c_atom  = {NAN, NAN, NAN};
                float ca_atom_bfactor;
                bool hasCA = false;
                for (gemmi::Atom &atom : res.atoms) {
                    if (atom.name == "CA") {
                        ca_atom.x = atom.pos.x;
                        ca_atom.y = atom.pos.y;
                        ca_atom.z = atom.pos.z;
                        ca_atom_bfactor = atom.b_iso;
                        hasCA = true;
                    } else if (atom.name == "CB") {
                        cb_atom.x = atom.pos.x;
                        cb_atom.y = atom.pos.y;
                        cb_atom.z = atom.pos.z;
                    } else if (atom.name == "N") {
                        n_atom.x = atom.pos.x;
                        n_atom.y = atom.pos.y;
                        n_atom.z = atom.pos.z;
                    } else if (atom.name == "C") {
                        c_atom.x = atom.pos.x;
                        c_atom.y = atom.pos.y;
                        c_atom.z = atom.pos.z;
                    }
                }
                if(hasCA == false){
                    continue;
                }
                ca_bfactor.push_back(ca_atom_bfactor);
                ca.push_back(ca_atom);
                cb.push_back(cb_atom);
                n.push_back(n_atom);
                c.push_back(c_atom);
                currPos++;
                if (threeAA2oneAA.find(res.name) == threeAA2oneAA.end()) {
                    ami.push_back('X');
                } else {
                    ami.push_back(threeAA2oneAA[res.name]);
                }
            }
            taxIds.push_back(taxId == -1 ? 0 : taxId);
            chain.push_back(std::make_pair(chainStartPos, currPos));
        }
    }
}
