//
//  Copyright (C) 2018-2021 Susan H. Leung and other RDKit contributors
//
//   @@ All Rights Reserved @@
//  This file is part of the RDKit.
//  The contents are covered by the terms of the BSD license
//  which is included in the file license.txt, found at the root
//  of the RDKit source tree.
//
#include <numeric>
#include "Charge.h"
#include <GraphMol/SmilesParse/SmilesParse.h>
#include <GraphMol/SmilesParse/SmilesWrite.h>
#include <GraphMol/new_canon.h>
#include <GraphMol/RDKitBase.h>
#include <GraphMol/Substruct/SubstructMatch.h>
#include <boost/range/adaptor/reversed.hpp>
#include <RDGeneral/BoostStartInclude.h>
#include <boost/flyweight.hpp>
#include <boost/flyweight/key_value.hpp>
#include <boost/flyweight/no_tracking.hpp>
#include <RDGeneral/BoostEndInclude.h>

namespace RDKit {
namespace MolStandardize {

// The default list of ChargeCorrections.
std::vector<ChargeCorrection> CHARGE_CORRECTIONS = {
    ChargeCorrection("[Li,Na,K]", "[Li,Na,K;X0+0]", 1),
    ChargeCorrection("[Mg,Ca]", "[Mg,Ca;X0+0]", 2),
    ChargeCorrection("[Cl]", "[Cl;X0+0]", -1)};

typedef boost::flyweight<
    boost::flyweights::key_value<std::string, AcidBaseCatalogParams>,
    boost::flyweights::no_tracking>
    param_filename_flyweight;

typedef boost::flyweight<
    boost::flyweights::key_value<
        std::vector<std::tuple<std::string, std::string, std::string>>,
        AcidBaseCatalogParams>,
    boost::flyweights::no_tracking>
    param_data_flyweight;

// constructor
Reionizer::Reionizer() {
  const AcidBaseCatalogParams *abparams =
      &(param_filename_flyweight(defaultCleanupParameters.acidbaseFile).get());
  this->d_abcat = new AcidBaseCatalog(abparams);
  this->d_ccs = CHARGE_CORRECTIONS;
}

Reionizer::Reionizer(const std::string acidbaseFile) {
  const AcidBaseCatalogParams *abparams =
      &(param_filename_flyweight(acidbaseFile).get());
  this->d_abcat = new AcidBaseCatalog(abparams);
  this->d_ccs = CHARGE_CORRECTIONS;
}

Reionizer::Reionizer(
    const std::vector<std::tuple<std::string, std::string, std::string>>
        &data) {
  const AcidBaseCatalogParams *abparams = &(param_data_flyweight(data).get());
  this->d_abcat = new AcidBaseCatalog(abparams);
  this->d_ccs = CHARGE_CORRECTIONS;
}

Reionizer::Reionizer(
    const std::vector<std::tuple<std::string, std::string, std::string>> &data,
    const std::vector<ChargeCorrection> ccs) {
  const AcidBaseCatalogParams *abparams = &(param_data_flyweight(data).get());
  this->d_abcat = new AcidBaseCatalog(abparams);
  this->d_ccs = ccs;
}

Reionizer::Reionizer(const std::string acidbaseFile,
                     const std::vector<ChargeCorrection> ccs) {
  const AcidBaseCatalogParams *abparams =
      &(param_filename_flyweight(acidbaseFile).get());
  this->d_abcat = new AcidBaseCatalog(abparams);
  this->d_ccs = ccs;
}

Reionizer::Reionizer(std::istream &acidbaseStream,
                     const std::vector<ChargeCorrection> ccs) {
  AcidBaseCatalogParams abparams(acidbaseStream);
  this->d_abcat = new AcidBaseCatalog(&abparams);
  this->d_ccs = ccs;
}

Reionizer::~Reionizer() { delete d_abcat; }

// Reionizer::Reionizer(const AcidBaseCatalog *abcat, const
// std::vector<ChargeCorrection> ccs = CHARGE_CORRECTIONS) 	:
// d_abcat(abcat),
// d_css(css) {};

ROMol *Reionizer::reionize(const ROMol &mol) {
  auto omol = new RWMol(mol);
  this->reionizeInPlace(*omol);
  return static_cast<ROMol *>(omol);
}

void Reionizer::reionizeInPlace(RWMol &mol) {
  PRECONDITION(this->d_abcat, "");
  const AcidBaseCatalogParams *abparams = this->d_abcat->getCatalogParams();

  PRECONDITION(abparams, "");
  const std::vector<std::pair<ROMOL_SPTR, ROMOL_SPTR>> abpairs =
      abparams->getPairs();

  if (mol.needsUpdatePropertyCache()) {
    mol.updatePropertyCache(false);
  }
  int start_charge = MolOps::getFormalCharge(mol);

  for (const auto &cc : this->d_ccs) {
    std::vector<MatchVectType> res;
    ROMOL_SPTR ccmol(SmartsToMol(cc.Smarts));
    unsigned int matches = SubstructMatch(mol, *ccmol, res);
    if (matches) {
      for (const auto &match : res) {
        for (const auto &pair : match) {
          auto idx = pair.second;
          Atom *atom = mol.getAtomWithIdx(idx);
          BOOST_LOG(rdInfoLog)
              << "Applying charge correction " << cc.Name << " "
              << atom->getSymbol() << " " << cc.Charge << "\n";
          atom->setFormalCharge(cc.Charge);
        }
      }
    }
  }
  int current_charge = MolOps::getFormalCharge(mol);
  int charge_diff = current_charge - start_charge;
  // std::cout << "Current charge: " << current_charge << std::endl;
  // std::cout << "Charge diff: " << charge_diff << std::endl;

  // If molecule is now neutral, assume everything is now fixed
  // But otherwise, if charge has become more positive,
  // look for additional protonated acid groups to ionize

  if (current_charge != 0) {
    while (charge_diff > 0) {
      // returns the acid strength ranking (ppos)
      // and the substruct match (poccur) in a pair
      std::shared_ptr<std::pair<unsigned int, std::vector<unsigned int>>> res(
          this->strongestProtonated(mol, abpairs));
      if (res == nullptr) {
        break;
      } else {
        unsigned int ppos = res->first;
        std::vector<unsigned int> poccur = res->second;
        std::string abname;
        std::pair<ROMOL_SPTR, ROMOL_SPTR> abpair = abpairs[ppos];
        (abpair.first)->getProp(common_properties::_Name, abname);
        BOOST_LOG(rdInfoLog) << "Ionizing " << abname
                             << " to balance previous charge corrections\n";
        Atom *patom = mol.getAtomWithIdx(poccur.back());
        patom->setFormalCharge(patom->getFormalCharge() - 1);

        if (patom->getNumExplicitHs() > 0) {
          patom->setNumExplicitHs(patom->getNumExplicitHs() - 1);
        }

        patom->updatePropertyCache();
        --charge_diff;
      }
    }
  }

  // std::cout << MolToSmiles(mol) << std::endl;
  // std::cout << "Charge diff: " << charge_diff << std::endl;

  std::set<std::vector<unsigned int>> already_moved;
  while (true) {
    std::shared_ptr<std::pair<unsigned int, std::vector<unsigned int>>> sp_res(
        this->strongestProtonated(mol, abpairs));
    std::shared_ptr<std::pair<unsigned int, std::vector<unsigned int>>> wi_res(
        this->weakestIonized(mol, abpairs));
    if (sp_res != nullptr && wi_res != nullptr) {
      unsigned int ppos = sp_res->first;
      unsigned int ipos = wi_res->first;
      std::vector<unsigned int> poccur = sp_res->second;
      std::vector<unsigned int> ioccur = wi_res->second;
      if (ppos < ipos) {
        if (poccur.back() == ioccur.back()) {
          // Bad! H wouldn't be moved, resulting in infinite loop.
          BOOST_LOG(rdInfoLog)
              << "Aborted reionization due to unexpected situation\n";
          break;
        }

        std::vector<unsigned int> key = {poccur.back(), ioccur.back()};
        std::sort(key.begin(), key.end());
        const bool is_in = already_moved.find(key) != already_moved.end();
        if (is_in) {
          BOOST_LOG(rdInfoLog)
              << "Aborting reionization to avoid infinite loop due \
								to it being ambiguous where to put a Hydrogen\n";
          break;
        }
        already_moved.insert(key);

        std::string prot_name, ionized_name;
        std::pair<ROMOL_SPTR, ROMOL_SPTR> prot_pair = abpairs[ppos];
        std::pair<ROMOL_SPTR, ROMOL_SPTR> ionized_pair = abpairs[ipos];
        (prot_pair.first)->getProp(common_properties::_Name, prot_name);
        (ionized_pair.first)->getProp(common_properties::_Name, ionized_name);

        BOOST_LOG(rdInfoLog) << "Moved proton from " << prot_name << " to "
                             << ionized_name << "\n";
        // Remove hydrogen from strongest protonated
        Atom *patom = mol.getAtomWithIdx(poccur.back());
        patom->setFormalCharge(patom->getFormalCharge() - 1);
        // If no implicit Hs to autoremove, and at least 1 explicit H to
        // remove,
        //  reduce explicit count by 1
        if (patom->getNumImplicitHs() == 0 && patom->getNumExplicitHs() > 0) {
          patom->setNumExplicitHs(patom->getNumExplicitHs() - 1);
          // TODO: Remove any chiral label on patom?
        }
        patom->updatePropertyCache();

        // Add hydrogen to weakest ionized
        Atom *iatom = mol.getAtomWithIdx(ioccur.back());
        iatom->setFormalCharge(iatom->getFormalCharge() + 1);
        // Increase explicit H count if no implicit, or aromatic N or P,
        // or non default valence state
        const PeriodicTable *table = PeriodicTable::getTable();
        INT_VECT valence_list = table->getValenceList(iatom->getAtomicNum());
        bool found =
            (std::find(valence_list.begin(), valence_list.end(),
                       iatom->getTotalValence()) != valence_list.end());
        if (iatom->getNoImplicit() ||
            ((patom->getAtomicNum() == 7 || patom->getAtomicNum() == 15) &&
             patom->getIsAromatic()) ||
            !found) {
          iatom->setNumExplicitHs(iatom->getNumExplicitHs() + 1);
        }
        iatom->updatePropertyCache();
      } else {
        break;
      }
    } else {
      break;
    }
  }  // while loop
}

std::pair<unsigned int, std::vector<unsigned int>>
    *Reionizer::strongestProtonated(
        const ROMol &mol,
        const std::vector<std::pair<ROMOL_SPTR, ROMOL_SPTR>> &abpairs) {
  // position is the position in the acid list.
  unsigned int position = 0;
  for (const auto &abpair : abpairs) {
    RDKit::MatchVectType res;
    unsigned int matches = SubstructMatch(mol, *(abpair.first), res);
    if (matches > 0) {
      std::vector<unsigned int> occurrence;
      for (const auto &pair : res) {
        occurrence.push_back(pair.second);
      }
      return new std::pair<unsigned int, std::vector<unsigned int>>(position,
                                                                    occurrence);
    }
    ++position;
  }
  return nullptr;
}

std::pair<unsigned int, std::vector<unsigned int>> *Reionizer::weakestIonized(
    const ROMol &mol,
    const std::vector<std::pair<ROMOL_SPTR, ROMOL_SPTR>> &abpairs) {
  // position is the position in the acid list.
  unsigned int position = 0;
  for (const auto &abpair : boost::adaptors::reverse(abpairs)) {
    RDKit::MatchVectType res;
    unsigned int matches = SubstructMatch(mol, *(abpair.second), res);
    if (matches > 0) {
      std::vector<unsigned int> occurrence;
      for (const auto &pair : res) {
        occurrence.push_back(pair.second);
      }
      return new std::pair<unsigned int, std::vector<unsigned int>>(
          (abpairs.size() - position - 1), occurrence);
    }
    ++position;
  }
  return nullptr;
}

Uncharger::Uncharger()
    : pos_h(SmartsToMol("[+,+2,+3,+4;!h0;!$(*~[-]),$(*(~[-])~[-])]")),
      pos_noh(SmartsToMol("[+,+2,+3,+4;h0;!$(*~[-])]")),
      neg(SmartsToMol("[-!$(*~[+,+2,+3,+4])]")),
      neg_acid(SmartsToMol(
          // carboxylate, carbonate, sulfi(a)te,
          // and their thio-analogues
          // (among other less likely structures)
          "[$([O,S;-][C,S;+0]=[O,S]),"
          // phosphi(a)te, nitrate
          // and their thio-analogues
          "$([O,S;-][N,P;+](=[O,S])[O,S;-]),"
          // hali(a)te, perhalate
          "$([O-][Cl,Br,I;+,+2,+3][O-]),"
          // tetrazole
          "$([n-]1nnnc1),$([n-]1ncnn1)]")){};

void neutralizeNeg(Atom *atom, int hDelta = 1) {
  atom->setNumExplicitHs(atom->getTotalNumHs() + hDelta);
  atom->setNoImplicit(true);
  atom->setFormalCharge(atom->getFormalCharge() + 1);
  BOOST_LOG(rdInfoLog) << "Removed negative charge.\n";
  // since we changed the number of explicit Hs, we need to update the
  // other valence parameters
  atom->updatePropertyCache(false);
}

bool neutralizeNegIfPossible(Atom *atom) {
  bool is_early_atom = isEarlyAtom(atom->getAtomicNum());
  bool has_hs = atom->getTotalNumHs();
  if (is_early_atom && !has_hs) {
    return false;
  }
  int hDelta = (is_early_atom ? -1 : 1);
  // Add hydrogen to negative atom, increase formal charge
  // Until quaternary positive == negative total or no more negative
  // acid
  neutralizeNeg(atom, hDelta);
  return true;
}

ROMol *Uncharger::uncharge(const ROMol &mol) {
  auto omol = new RWMol(mol);
  this->unchargeInPlace(*omol);
  return static_cast<ROMol *>(omol);
}
void Uncharger::unchargeInPlace(RWMol &mol) {
  BOOST_LOG(rdInfoLog) << "Running Uncharger\n";
  if (mol.needsUpdatePropertyCache()) {
    mol.updatePropertyCache(false);
  }
  std::vector<MatchVectType> p_matches;
  std::vector<MatchVectType> q_matches;
  std::vector<MatchVectType> n_matches;
  std::vector<MatchVectType> a_matches;

  // Get atom ids for matches
  SubstructMatch(mol, *(this->pos_h), p_matches);
  SubstructMatch(mol, *(this->pos_noh), q_matches);
  unsigned int q_matched = 0;
  for (const auto &match : q_matches) {
    q_matched += mol.getAtomWithIdx(match[0].second)->getFormalCharge();
  }
  unsigned int n_matched = SubstructMatch(mol, *(this->neg), n_matches);
  unsigned int a_matched = SubstructMatch(mol, *(this->neg_acid), a_matches);

  bool needsNeutralization =
      (q_matched > 0 && (n_matched > 0 || a_matched > 0));
  std::vector<unsigned int> atomRanks(mol.getNumAtoms());
  if (df_canonicalOrdering && needsNeutralization) {
    Canon::rankMolAtoms(mol, atomRanks);
  } else {
    std::iota(atomRanks.begin(), atomRanks.end(), 0);
  }
  auto getRankIdxPair = [&atomRanks](const MatchVectType &mv) {
    int aidx = mv.front().second;
    return std::make_pair(atomRanks[aidx], aidx);
  };
  std::vector<std::pair<int, int>> n_atoms;
  n_atoms.reserve(n_matches.size());
  std::transform(n_matches.begin(), n_matches.end(),
                 std::back_inserter(n_atoms), getRankIdxPair);
  std::vector<std::pair<int, int>> a_atoms;
  a_atoms.reserve(a_matches.size());
  std::transform(a_matches.begin(), a_matches.end(),
                 std::back_inserter(a_atoms), getRankIdxPair);
  if (df_canonicalOrdering) {
    std::sort(n_atoms.begin(), n_atoms.end());
    std::sort(a_atoms.begin(), a_atoms.end());
  }

  // merge n_atoms and a_atoms into one single list of 
  // negatively charged sites that will be neutralized in
  // sequence
  std::vector<std::pair<int, int>> neg_atoms;
  neg_atoms.reserve(n_atoms.size() + a_atoms.size());

  // insert the elements from n_atoms first, but skip those
  // that also appear in a_atoms and will be considered next
  boost::dynamic_bitset<> nonAcids(mol.getNumAtoms());
  nonAcids.set();
  for (const auto &pair : a_atoms) {
    nonAcids.reset(pair.second);
  }
  for (const auto &pair : n_atoms) {
    unsigned int idx = pair.second;
    if (!nonAcids[idx]) {
      continue;
    }
    neg_atoms.push_back(pair);
  }

  // insert the elements from a_atoms, but make sure that
  // the anions of monoprotic acids are not protonated multiple
  // times
  boost::dynamic_bitset<> skipChargeSep(mol.getNumAtoms());
  for (const auto &pair : a_atoms) {
    unsigned int idx = pair.second;
    Atom *atom = mol.getAtomWithIdx(idx);
    for (const auto &nbri :
          boost::make_iterator_range(mol.getAtomNeighbors(atom))) {
      const auto &nbr = (mol)[nbri];
      auto nbrIdx = nbr->getIdx();
      // if the neighbor has a positive charge,
      // neutralize only once (e.g., NO3-)
      if (nbr->getFormalCharge() > 0) {
        if (!skipChargeSep.test(nbrIdx)) {
          skipChargeSep.set(nbrIdx);
        } else {
          skipChargeSep.set(idx);
        }
        break;
      }
    }
  }
  for (const auto &pair : a_atoms) {
    unsigned int idx = pair.second;
    if (skipChargeSep.test(idx)) {
      continue;
    }
    neg_atoms.push_back(pair);
  }

  // Surplus negative charges (initially estimated as the total amount of
  // neutralizable negative charge).
  int neg_surplus = neg_atoms.size();
  if (!df_force) {
    // unless we want to fully uncharge the compound, the estimated surplus must
    // be deduced the amount of positive charge that is not possible to neutralize
    // and must be balanced.
    neg_surplus -= q_matched;
  }

  // Neutralize surplus negative charges
  if (neg_surplus) {
    for (const auto &pair : neg_atoms) {
      unsigned int idx = pair.second;
      Atom *atom = mol.getAtomWithIdx(idx);
      if (neutralizeNegIfPossible(atom) && !--neg_surplus) {
        break;
      }
    }
  }

  // Neutralize cations until there is no longer a net charge remaining:
  int netCharge = 0;
  for (const auto &at : mol.atoms()) {
    netCharge += at->getFormalCharge();
  }

  if (netCharge > 0) {
    // Neutralize positive charges where H counts can be adjusted
    std::vector<unsigned int> p_idx_matches;
    for (const auto &match : p_matches) {
      for (const auto &pair : match) {
        p_idx_matches.push_back(pair.second);
      }
    }
    for (const auto &idx : p_idx_matches) {
      Atom *atom = mol.getAtomWithIdx(idx);
      // atoms from places like Mol blocks are normally missing explicit Hs:
      atom->setNumExplicitHs(atom->getTotalNumHs());
      atom->setNoImplicit(true);
      while (atom->getFormalCharge() > 0 && netCharge > 0) {
        atom->setFormalCharge(atom->getFormalCharge() - 1);
        --netCharge;
        // the special case for C here was github #2792
        if (atom->getAtomicNum() != 6 && !isEarlyAtom(atom->getAtomicNum())) {
          auto nExplicit = atom->getNumExplicitHs();
          if (nExplicit >= 1) {
            atom->setNumExplicitHs(nExplicit - 1);
          }
          if (nExplicit == 1) {
            // we just removed the last one:
            break;
          }
        } else {
          atom->setNumExplicitHs(atom->getNumExplicitHs() + 1);
        }
        BOOST_LOG(rdInfoLog) << "Removed positive charge.\n";
        // since we changed the number of explicit Hs, we need to update the
        // other valence parameters
        atom->updatePropertyCache(false);
      }
      if (!netCharge) {
        break;
      }
    }
  }
}  // namespace MolStandardize

}  // namespace MolStandardize
}  // namespace RDKit
