#include "FWCore/Framework/interface/Frameworkfwd.h"
#include "FWCore/Framework/interface/ESHandle.h"
#include "FWCore/Framework/interface/EventSetup.h"


#include "MNTriggerStudies/MNTriggerAna/interface/JetView.h"
#include <boost/foreach.hpp>
#include <boost/algorithm/string.hpp>
#include <cmath>
#include <sstream>
#include <algorithm>
//#include "JetMETCorrections/Objects/interface/JetCorrectionsRecord.h"
//#include "DataFormats/FWLite/interface/EventSetup.h"
//#include "FWCore/Framework/interface/ESHandle.h"
#include "CondFormats/JetMETObjects/interface/JetCorrectorParameters.h"
//#include "JetMETCorrections/Objects/interface/JetCorrector.h"
#include "JetMETCorrections/Objects/interface/JetCorrectionsRecord.h"



// https://twiki.cern.ch/twiki/bin/view/CMSPublic/SWGuidePhysicsCutParser - should I ?

namespace xx {
    float stof(std::string s){
        std::stringstream ss(s);
        float ret;
        ss >> ret;
        return ret;
    }

    struct TempJetHolder {
        reco::Candidate::LorentzVector p4;
        reco::Candidate::LorentzVector p4Gen;
        int jetId;
    };

}




JetView::JetView(const edm::ParameterSet& iConfig, TTree * tree):
pfJetID(PFJetIDSelectionFunctor::FIRSTDATA, PFJetIDSelectionFunctor::LOOSE),
caloJetID(JetIDSelectionFunctor::PURE09,  JetIDSelectionFunctor::LOOSE),
m_jecUnc(0)

{
    registerVecP4("recoTracks", tree);
    registerVecFloat("dz", tree);
    registerVecFloat("dxy", tree);

    m_maxEta = iConfig.getParameter<double>("maxEta");
    m_minPt = iConfig.getParameter<double>("minPt");
    m_maxnum = iConfig.getParameter<double>("maxnum"); // save maxnum hardest jets

    m_inputCol = iConfig.getParameter<edm::InputTag>("input");
    m_variations = iConfig.getParameter<std::vector<std::string> >("variations"); // central, _jecUp/Down, _jerUp/Down
    std::set<std::string> knownVars;
    knownVars.insert("central");
    knownVars.insert("jecUp");
    knownVars.insert("jecDown");
    knownVars.insert("jerUp");
    knownVars.insert("jerDown");
    // TODO register branches
    BOOST_FOREACH( std::string s, m_variations){
        if (knownVars.find(s)==knownVars.end()){
            throw "Variation not known "+s + "\n";
        }
    }

    std::vector<std::string> JERdesc = iConfig.getParameter<std::vector<std::string> >("jerFactors");
    BOOST_FOREACH( std::string s, JERdesc){
        std::vector<std::string> floatsAsStrings;
        boost::split(floatsAsStrings, s, boost::is_any_of("\t "));
        //
        // 0.5 1.052 0.012 0.062 0.061
        if (floatsAsStrings.size() != 5) {
            throw "Wrong size of JER factors string\n";
        }
            float etaMax = xx::stof(floatsAsStrings[0]);
            float jer = xx::stof(floatsAsStrings[1]);
            float err = xx::stof(floatsAsStrings[2]);
            float errUp = xx::stof(floatsAsStrings[3]);
            float errDown  = xx::stof(floatsAsStrings[4]);
            float jerUp   = jer + sqrt(err*err+errUp*errUp);
            float jerDown = jer - sqrt(err*err+errDown*errDown);
            std::vector<float> JER;
            JER.push_back(etaMax);
            JER.push_back(jer);
            JER.push_back(jerUp);
            JER.push_back(jerDown);
            m_JER.push_back(JER);
            //print "JER factors:", etaMax, jer, jerUp, jerDown, "|", err, errUp, errDown
    }

}


void JetView::fillSpecific(const edm::Event& iEvent, const edm::EventSetup& iSetup){
    edm::Handle<pat::JetCollection> hJets;
    iEvent.getByLabel(m_inputCol, hJets);

    if (m_jecUnc == 0 && hJets->size()>0 ) { // couldnt find better place..
        edm::ESHandle<JetCorrectorParametersCollection> JetCorParColl;
        std::string payload("AK5PF");
        if (hJets->at(0).isCaloJet()) {
            payload == "AK5Calo";
        }
        iSetup.get<JetCorrectionsRecord>().get(payload,JetCorParColl); 
        JetCorrectorParameters const & JetCorPar = (*JetCorParColl)["Uncertainty"];
        m_jecUnc = new JetCorrectionUncertainty(JetCorPar);
    }


    // save indices of jets, that have non-nonsense JEC (no negative values)
    std::vector<int> goodJets;
    for (unsigned int i = 0; i<hJets->size(); ++i){

        std::vector<std::string> jecs = hJets->at(i).availableJECLevels(0);

        bool badJet = false;
        for (unsigned int aa = 0; aa < jecs.size(); ++aa){
            if (hJets->at(i).jecFactor(jecs[aa])<0) {
                badJet = true;
                break;
            }
        }
        if (badJet) continue;
        goodJets.push_back(i);
    }

    BOOST_FOREACH( std::string s, m_variations){
        BOOST_FOREACH(int i, goodJets){
            xx::TempJetHolder t;
            t.p4Gen = reco::Candidate::LorentzVector();
            if (hJets->at(i).genJet()){
               t.p4Gen = hJets->at(i).genJet()->p4();
            }
            t.jetId = jetID(hJets->at(i), iEvent);


        }
    }


}

reco::Candidate::LorentzVector JetView::getMomentum(const pat::Jet & jet, std::string variation) {
    reco::Candidate::LorentzVector gen;
    if (jet.genJet()){
       gen  = jet.genJet()->p4();
    }
    // at this point jet momentum has JEC fully applied
    if (variation == "central" or  variation.find("jer") != std::string::npos) {
        return smear(gen, jet.p4(), variation);
    }
    else if (  variation.find("jec") != std::string::npos) {
        // Note: first apply JEC shift, than central JER shift
        reco::Candidate::LorentzVector jecShifted = shiftJEC(jet.p4(), variation);
        return smear(gen, jecShifted, variation);
    }

    throw "Variation not known: " + variation + "\n";
    return jet.p4();
} 

reco::Candidate::LorentzVector JetView::smear(const reco::Candidate::LorentzVector & gen, 
                                              const reco::Candidate::LorentzVector & rec,
                                              std::string variation)
{
    if (gen.pt() < 0.01) return rec; // no gen jet, no smear possible
    if (rec.pt() < 0.01) return rec; // no rec jet, possible in some cases when JEC variation applied earlier

    float eta = std::abs(rec.eta());
    if (eta > 5) return rec; // no smearing factors for higher values
    float factor = -1;
    for(unsigned int i = 0; i < m_JER.size(); ++i){
        if (eta < m_JER[i][0]){
            if (variation == "central") factor = m_JER[i].at(1);
            else if (variation == "jerDown") factor = m_JER[i].at(2);
            else if (variation == "jerUp") factor = m_JER[i].at(3);
            else throw "Unexpected variation: "+ variation + "\n";
            break;
        }
    }
    if (factor < 0){
        throw "Cannot determine JER factor for jet\n";
    }

    float ptRec = rec.pt();
    float ptGen = gen.pt();
    float diff = ptRec-ptGen;
    float ptRet = std::max(float(0.), ptGen+factor*diff);
    if (ptRet < 0.01){
        return  reco::Candidate::LorentzVector();
    }

    float scaleFactor = ptRet/ptRec;
    return rec*scaleFactor;

}

reco::Candidate::LorentzVector JetView::shiftJEC(const reco::Candidate::LorentzVector &rec,  std::string variation) {
    if (variation != "jecUp" and variation != "jecDown"){
        throw "JetView::shiftJEC:  Unecpected variation " + variation + "\n";
    }

    m_jecUnc->setJetEta(rec.eta());
    m_jecUnc->setJetPt(rec.eta());
    float unc = m_jecUnc->getUncertainty(true);
    float ptFactor = 1.;
    if (variation == "jecDown") ptFactor = -1;
    float factor = 1. + ptFactor*unc;
    if (factor <= 0) return reco::Candidate::LorentzVector();
    return rec*factor;

}





int JetView::jetID(const pat::Jet & jet, const edm::Event& iEvent) {
    int ret = 1;
    if (jet.isCaloJet()) {
        // TODO
    } else if (jet.isPFJet()) {
        pat::strbitset bs = pfJetID.getBitTemplate(); 
        if (!pfJetID(jet, bs)) ret = 0;
    }

    return ret;

}


