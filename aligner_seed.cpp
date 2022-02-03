/*
 * Copyright 2011, Ben Langmead <langmea@cs.jhu.edu>
 *
 * This file is part of Bowtie 2.
 *
 * Bowtie 2 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Bowtie 2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Bowtie 2.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "aligner_cache.h"
#include "aligner_seed.h"
#include "search_globals.h"
#include "bt2_idx.h"

using namespace std;

/**
 * Construct a constraint with no edits of any kind allowed.
 */
Constraint Constraint::exact() {
	Constraint c;
	c.edits = c.mms = c.ins = c.dels = c.penalty = 0;
	return c;
}

/**
 * Construct a constraint where the only constraint is a total
 * penalty constraint.
 */
Constraint Constraint::penaltyBased(int pen) {
	Constraint c;
	c.penalty = pen;
	return c;
}

/**
 * Construct a constraint where the only constraint is a total
 * penalty constraint related to the length of the read.
 */
Constraint Constraint::penaltyFuncBased(const SimpleFunc& f) {
	Constraint c;
	c.penFunc = f;
	return c;
}

/**
 * Construct a constraint where the only constraint is a total
 * penalty constraint.
 */
Constraint Constraint::mmBased(int mms) {
	Constraint c;
	c.mms = mms;
	c.edits = c.dels = c.ins = 0;
	return c;
}

/**
 * Construct a constraint where the only constraint is a total
 * penalty constraint.
 */
Constraint Constraint::editBased(int edits) {
	Constraint c;
	c.edits = edits;
	c.dels = c.ins = c.mms = 0;
	return c;
}

//
// Some static methods for constructing some standard SeedPolicies
//

/**
 * Given a read, depth and orientation, extract a seed data structure
 * from the read and fill in the steps & zones arrays.  The Seed
 * contains the sequence and quality values.
 */
bool
Seed::instantiate(
	const Read& read,
	const BTDnaString& seq, // seed read sequence
	const BTString& qual,   // seed quality sequence
	const Scoring& pens,
	int depth,
	int seedoffidx,
	int seedtypeidx,
	bool fw,
	InstantiatedSeed& is) const
{
	assert(overall != NULL);
	int seedlen = len;
	if((int)read.length() < seedlen) {
		// Shrink seed length to fit read if necessary
		seedlen = (int)read.length();
	}
	assert_gt(seedlen, 0);
	is.steps.resize(seedlen);
	is.zones.resize(seedlen);
	// Fill in 'steps' and 'zones'
	//
	// The 'steps' list indicates which read character should be
	// incorporated at each step of the search process.  Often we will
	// simply proceed from one end to the other, in which case the
	// 'steps' list is ascending or descending.  In some cases (e.g.
	// the 2mm case), we might want to switch directions at least once
	// during the search, in which case 'steps' will jump in the
	// middle.  When an element of the 'steps' list is negative, this
	// indicates that the next
	//
	// The 'zones' list indicates which zone constraint is active at
	// each step.  Each element of the 'zones' list is a pair; the
	// first pair element indicates the applicable zone when
	// considering either mismatch or delete (ref gap) events, while
	// the second pair element indicates the applicable zone when
	// considering insertion (read gap) events.  When either pair
	// element is a negative number, that indicates that we are about
	// to leave the zone for good, at which point we may need to
	// evaluate whether we have reached the zone's budget.
	//
	switch(type) {
		case SEED_TYPE_EXACT: {
			for(int k = 0; k < seedlen; k++) {
				is.steps[k] = -(seedlen - k);
				// Zone 0 all the way
				is.zones[k].first = is.zones[k].second = 0;
			}
			break;
		}
		case SEED_TYPE_LEFT_TO_RIGHT: {
			for(int k = 0; k < seedlen; k++) {
				is.steps[k] = k+1;
				// Zone 0 from 0 up to ceil(len/2), then 1
				is.zones[k].first = is.zones[k].second = ((k < (seedlen+1)/2) ? 0 : 1);
			}
			// Zone 1 ends at the RHS
			is.zones[seedlen-1].first = is.zones[seedlen-1].second = -1;
			break;
		}
		case SEED_TYPE_RIGHT_TO_LEFT: {
			for(int k = 0; k < seedlen; k++) {
				is.steps[k] = -(seedlen - k);
				// Zone 0 from 0 up to floor(len/2), then 1
				is.zones[k].first  = ((k < seedlen/2) ? 0 : 1);
				// Inserts: Zone 0 from 0 up to ceil(len/2)-1, then 1
				is.zones[k].second = ((k < (seedlen+1)/2+1) ? 0 : 1);
			}
			is.zones[seedlen-1].first = is.zones[seedlen-1].second = -1;
			break;
		}
		case SEED_TYPE_INSIDE_OUT: {
			// Zone 0 from ceil(N/4) up to N-floor(N/4)
			int step = 0;
			for(int k = (seedlen+3)/4; k < seedlen - (seedlen/4); k++) {
				is.zones[step].first = is.zones[step].second = 0;
				is.steps[step++] = k+1;
			}
			// Zone 1 from N-floor(N/4) up
			for(int k = seedlen - (seedlen/4); k < seedlen; k++) {
				is.zones[step].first = is.zones[step].second = 1;
				is.steps[step++] = k+1;
			}
			// No Zone 1 if seedlen is short (like 2)
			//assert_eq(1, is.zones[step-1].first);
			is.zones[step-1].first = is.zones[step-1].second = -1;
			// Zone 2 from ((seedlen+3)/4)-1 down to 0
			for(int k = ((seedlen+3)/4)-1; k >= 0; k--) {
				is.zones[step].first = is.zones[step].second = 2;
				is.steps[step++] = -(k+1);
			}
			assert_eq(2, is.zones[step-1].first);
			is.zones[step-1].first = is.zones[step-1].second = -2;
			assert_eq(seedlen, step);
			break;
		}
		default:
			throw 1;
	}
	// Instantiate constraints
	for(int i = 0; i < 3; i++) {
		is.cons[i] = zones[i];
		is.cons[i].instantiate(read.length());
	}
	is.overall = *overall;
	is.overall.instantiate(read.length());
	// Take a sweep through the seed sequence.  Consider where the Ns
	// occur and how zones are laid out.  Calculate the maximum number
	// of positions we can jump over initially (e.g. with the ftab) and
	// perhaps set this function's return value to false, indicating
	// that the arrangements of Ns prevents the seed from aligning.
	bool streak = true;
	is.maxjump = 0;
	bool ret = true;
	bool ltr = (is.steps[0] > 0); // true -> left-to-right
	for(size_t i = 0; i < is.steps.size(); i++) {
		assert_neq(0, is.steps[i]);
		int off = is.steps[i];
		off = abs(off)-1;
		Constraint& cons = is.cons[abs(is.zones[i].first)];
		int c = seq[off];  assert_range(0, 4, c);
		int q = qual[off];
		if(ltr != (is.steps[i] > 0) || // changed direction
		   is.zones[i].first != 0 ||   // changed zone
		   is.zones[i].second != 0)    // changed zone
		{
			streak = false;
		}
		if(c == 4) {
			// Induced mismatch
			if(cons.canN(q, pens)) {
				cons.chargeN(q, pens);
			} else {
				// Seed disqualified due to arrangement of Ns
				return false;
			}
		}
		if(streak) is.maxjump++;
	}
	is.seedoff = depth;
	is.seedoffidx = seedoffidx;
	is.fw = fw;
	is.s = *this;
	return ret;
}

/**
 * Return a set consisting of 1 seed encapsulating an exact matching
 * strategy.
 */
void
Seed::zeroMmSeeds(int ln, EList<Seed>& pols, Constraint& oall) {
	oall.init();
	// Seed policy 1: left-to-right search
	pols.expand();
	pols.back().len = ln;
	pols.back().type = SEED_TYPE_EXACT;
	pols.back().zones[0] = Constraint::exact();
	pols.back().zones[1] = Constraint::exact();
	pols.back().zones[2] = Constraint::exact();
	pols.back().overall = &oall;
}

/**
 * Return a set of 2 seeds encapsulating a half-and-half 1mm strategy.
 */
void
Seed::oneMmSeeds(int ln, EList<Seed>& pols, Constraint& oall) {
	oall.init();
	// Seed policy 1: left-to-right search
	pols.expand();
	pols.back().len = ln;
	pols.back().type = SEED_TYPE_LEFT_TO_RIGHT;
	pols.back().zones[0] = Constraint::exact();
	pols.back().zones[1] = Constraint::mmBased(1);
	pols.back().zones[2] = Constraint::exact(); // not used
	pols.back().overall = &oall;
	// Seed policy 2: right-to-left search
	pols.expand();
	pols.back().len = ln;
	pols.back().type = SEED_TYPE_RIGHT_TO_LEFT;
	pols.back().zones[0] = Constraint::exact();
	pols.back().zones[1] = Constraint::mmBased(1);
	pols.back().zones[1].mmsCeil = 0;
	pols.back().zones[2] = Constraint::exact(); // not used
	pols.back().overall = &oall;
}

/**
 * Return a set of 3 seeds encapsulating search roots for:
 *
 * 1. Starting from the left-hand side and searching toward the
 *    right-hand side allowing 2 mismatches in the right half.
 * 2. Starting from the right-hand side and searching toward the
 *    left-hand side allowing 2 mismatches in the left half.
 * 3. Starting (effectively) from the center and searching out toward
 *    both the left and right-hand sides, allowing one mismatch on
 *    either side.
 *
 * This is not exhaustive.  There are 2 mismatch cases mised; if you
 * imagine the seed as divided into four successive quarters A, B, C
 * and D, the cases we miss are when mismatches occur in A and C or B
 * and D.
 */
void
Seed::twoMmSeeds(int ln, EList<Seed>& pols, Constraint& oall) {
	oall.init();
	// Seed policy 1: left-to-right search
	pols.expand();
	pols.back().len = ln;
	pols.back().type = SEED_TYPE_LEFT_TO_RIGHT;
	pols.back().zones[0] = Constraint::exact();
	pols.back().zones[1] = Constraint::mmBased(2);
	pols.back().zones[2] = Constraint::exact(); // not used
	pols.back().overall = &oall;
	// Seed policy 2: right-to-left search
	pols.expand();
	pols.back().len = ln;
	pols.back().type = SEED_TYPE_RIGHT_TO_LEFT;
	pols.back().zones[0] = Constraint::exact();
	pols.back().zones[1] = Constraint::mmBased(2);
	pols.back().zones[1].mmsCeil = 1; // Must have used at least 1 mismatch
	pols.back().zones[2] = Constraint::exact(); // not used
	pols.back().overall = &oall;
	// Seed policy 3: inside-out search
	pols.expand();
	pols.back().len = ln;
	pols.back().type = SEED_TYPE_INSIDE_OUT;
	pols.back().zones[0] = Constraint::exact();
	pols.back().zones[1] = Constraint::mmBased(1);
	pols.back().zones[1].mmsCeil = 0; // Must have used at least 1 mismatch
	pols.back().zones[2] = Constraint::mmBased(1);
	pols.back().zones[2].mmsCeil = 0; // Must have used at least 1 mismatch
	pols.back().overall = &oall;
}

/**
 * Types of actions that can be taken by the SeedAligner.
 */
enum {
	SA_ACTION_TYPE_RESET = 1,
	SA_ACTION_TYPE_SEARCH_SEED, // 2
	SA_ACTION_TYPE_FTAB,        // 3
	SA_ACTION_TYPE_FCHR,        // 4
	SA_ACTION_TYPE_MATCH,       // 5
	SA_ACTION_TYPE_EDIT         // 6
};

/**
 * Given a read and a few coordinates that describe a substring of the read (or
 * its reverse complement), fill in 'seq' and 'qual' objects with the seed
 * sequence and qualities.
 *
 * The seq field is filled with the sequence as it would align to the Watson
 * reference strand.  I.e. if fw is false, then the sequence that appears in
 * 'seq' is the reverse complement of the raw read substring.
 */
void
SeedAligner::instantiateSeq(
	const Read& read, // input read
	BTDnaString& seq, // output sequence
	BTString& qual,   // output qualities
	int len,          // seed length
	int depth,        // seed's 0-based offset from 5' end
	bool fw) const    // seed's orientation
{
	// Fill in 'seq' and 'qual'
	int seedlen = len;
	if((int)read.length() < seedlen) seedlen = (int)read.length();
	seq.resize(len);
	qual.resize(len);
	// If fw is false, we take characters starting at the 3' end of the
	// reverse complement of the read.
	for(int i = 0; i < len; i++) {
		seq.set(read.patFw.windowGetDna(i, fw, depth, len), i);
		qual.set(read.qual.windowGet(i, fw, depth, len), i);
	}
}

/**
 * We assume that all seeds are the same length.
 *
 * For each seed, instantiate the seed, retracting if necessary.
 */
pair<int, int> SeedAligner::instantiateSeeds(
	const EList<Seed>& seeds,  // search seeds
	size_t off,                // offset into read to start extracting
	int per,                   // interval between seeds
	const Read& read,          // read to align
	const Scoring& pens,       // scoring scheme
	bool nofw,                 // don't align forward read
	bool norc,                 // don't align revcomp read
	AlignmentCacheIface& cache,// holds some seed hits from previous reads
	SeedResults& sr,           // holds all the seed hits
	SeedSearchMetrics& met,    // metrics
	pair<int, int>& instFw,
	pair<int, int>& instRc)
{
	assert(!seeds.empty());
	assert_gt(read.length(), 0);
	// Check whether read has too many Ns
	offIdx2off_.clear();
	int len = seeds[0].len; // assume they're all the same length
#ifndef NDEBUG
	for(size_t i = 1; i < seeds.size(); i++) {
		assert_eq(len, seeds[i].len);
	}
#endif
	// Calc # seeds within read interval
	int nseeds = 1;
	if((int)read.length() - (int)off > len) {
		nseeds += ((int)read.length() - (int)off - len) / per;
	}
	for(int i = 0; i < nseeds; i++) {
		offIdx2off_.push_back(per * i + (int)off);
	}
	pair<int, int> ret;
	ret.first = 0;  // # seeds that require alignment
	ret.second = 0; // # seeds that hit in cache with non-empty results
	sr.reset(read, offIdx2off_, nseeds);
	assert(sr.repOk(&cache.current(), true)); // require that SeedResult be initialized
	// For each seed position
	for(int fwi = 0; fwi < 2; fwi++) {
		bool fw = (fwi == 0);
		if((fw && nofw) || (!fw && norc)) {
			// Skip this orientation b/c user specified --nofw or --norc
			continue;
		}
		// For each seed position
		for(int i = 0; i < nseeds; i++) {
			int depth = i * per + (int)off;
			int seedlen = seeds[0].len;
			// Extract the seed sequence at this offset
			// If fw == true, we extract the characters from i*per to
			// i*(per-1) (exclusive).  If fw == false, 
			instantiateSeq(
				read,
				sr.seqs(fw)[i],
				sr.quals(fw)[i],
				std::min<int>((int)seedlen, (int)read.length()),
				depth,
				fw);
			QKey qk(sr.seqs(fw)[i] ASSERT_ONLY(, tmpdnastr_));
			// For each search strategy
			EList<InstantiatedSeed>& iss = sr.instantiatedSeeds(fw, i);
			for(int j = 0; j < (int)seeds.size(); j++) {
				iss.expand();
				assert_eq(seedlen, seeds[j].len);
				InstantiatedSeed* is = &iss.back();
				if(seeds[j].instantiate(
					read,
					sr.seqs(fw)[i],
					sr.quals(fw)[i],
					pens,
					depth,
					i,
					j,
					fw,
					*is))
				{
					// Can we fill this seed hit in from the cache?
					ret.first++;
					if(fwi == 0) { instFw.first++; } else { instRc.first++; }
				} else {
					// Seed may fail to instantiate if there are Ns
					// that prevent it from matching
					met.filteredseed++;
					iss.pop_back();
				}
			}
		}
	}
	return ret;
}

// Update metrics as part of searchAllSeeds
static void finalizeOneAllSeeds(
	const uint64_t bwops,	     // Burrows-Wheeler operations
	const uint64_t bwedits,      // Burrows-Wheeler edits
	const uint64_t possearches,
	const uint64_t seedsearches,
	const uint64_t intrahits,
	const uint64_t interhits,
	const uint64_t ooms,
	const SeedResults& sr,       // holds all the seed hits
	SeedSearchMetrics& met,      // metrics
	PerReadMetrics& prm)         // per-read metrics
{
	prm.nSeedRanges = sr.numRanges();
	prm.nSeedElts = sr.numElts();
	prm.nSeedRangesFw = sr.numRangesFw();
	prm.nSeedRangesRc = sr.numRangesRc();
	prm.nSeedEltsFw = sr.numEltsFw();
	prm.nSeedEltsRc = sr.numEltsRc();
	prm.seedMedian = (uint64_t)(sr.medianHitsPerSeed() + 0.5);
	prm.seedMean = (uint64_t)sr.averageHitsPerSeed();

	prm.nSdFmops += bwops;
	met.seedsearch += seedsearches;
	met.nrange += sr.numRanges();
	met.nelt += sr.numElts();
	met.possearch += possearches;
	met.intrahit += intrahits;
	met.interhit += interhits;
	met.ooms += ooms;
	met.bwops += bwops;
	met.bweds += bwedits;
}

void SeedAligner::setRead(
	const Ebwt* ebwtFw,          // BWT index
	const Ebwt* ebwtBw,          // BWT' index
	const Read& read,            // read to align
	const Scoring& pens,         // scoring scheme
	AlignmentCacheIface& cache)  // local cache for seed alignments
{
	ebwtFw_ = ebwtFw;
	ebwtBw_ = ebwtBw;
	sc_ = &pens;
	read_ = &read;
	ca_ = &cache;
	bwops_ = bwedits_ = 0;
}

int SeedAligner::setSeedResult(
	SeedResults& sr,             // holds all the seed hits
	QVal &qv,                    // range of ranges in cache
	bool fw,                     // orientation of seed currently being searched
	size_t i)
{
			seq_  = &sr.seqs(fw)[i];  // seed sequence
			qual_ = &sr.quals(fw)[i]; // seed qualities
			off_  = sr.idx2off(i);    // seed offset (from 5')
			fw_   = fw;               // seed orientation
			// Tell the cache that we've started aligning, so the cache can
			// expect a series of on-the-fly updates
			int ret = ca_->beginAlign(*seq_, *qual_, qv);
			ASSERT_ONLY(hits_.clear());
			return ret;
}

/**
 * We assume that all seeds are the same length.
 *
 * For each seed:
 *
 * 1. Instantiate all seeds, retracting them if necessary.
 * 2. Calculate zone boundaries for each seed
 */
void SeedAligner::searchAllSeeds(
	const EList<Seed>& seeds,    // search seeds
	const Ebwt* ebwtFw,          // BWT index
	const Ebwt* ebwtBw,          // BWT' index
	const Read& read,            // read to align
	const Scoring& pens,         // scoring scheme
	AlignmentCacheIface& cache,  // local cache for seed alignments
	SeedResults& sr,             // holds all the seed hits
	SeedSearchMetrics& met,      // metrics
	PerReadMetrics& prm)         // per-read metrics
{
	assert(!seeds.empty());
	assert(ebwtFw != NULL);
	assert(ebwtFw->isInMemory());
	assert(sr.repOk(&cache.current()));
	setRead(ebwtFw, ebwtBw, read, pens, cache);
	uint64_t possearches = 0, seedsearches = 0, intrahits = 0, interhits = 0, ooms = 0;
	// For each instantiated seed
	for(int i = 0; i < (int)sr.numOffs(); i++) {
		for(int fwi = 0; fwi < 2; fwi++) {
			bool fw = (fwi == 0);
			assert(sr.repOk(&cache.current()));
			EList<InstantiatedSeed>& iss = sr.instantiatedSeeds(fw, i);
			if(iss.empty()) {
				// Cache hit in an across-read cache
				continue;
			}
			QVal qv;
			int ret = setSeedResult(sr, qv, fw, i);
			if(ret == -1) {
				// Out of memory when we tried to add key to map
				ooms++;
				continue;
			}
			bool abort = false;
			if(ret == 0) {
				// Not already in cache
				assert(cache.aligning());
				possearches++;
				for(size_t j = 0; j < iss.size(); j++) {
					// Set seq_ and qual_ appropriately, using the seed sequences
					// and qualities already installed in SeedResults
					assert_eq(fw, iss[j].fw);
					assert_eq(i, (int)iss[j].seedoffidx);
					s_ = &iss[j];
					// Do the search with respect to seq_, qual_ and s_.
					if(!searchSeedBi()) {
						// Memory exhausted during search
						ooms++;
						abort = true;
						break;
					}
					seedsearches++;
					assert(cache.aligning());
				}
				if(!abort) {
					qv = cache.finishAlign();
				}
			} else {
				// Already in cache
				assert_eq(1, ret);
				assert(qv.valid());
				intrahits++;
			}
			assert(abort || !cache.aligning());
			if(qv.valid()) {
				sr.add(
					qv,    // range of ranges in cache
					cache.current(), // cache
					i,     // seed index (from 5' end)
					fw);   // whether seed is from forward read
			}
		}
	}
	finalizeOneAllSeeds(
			bwops_, bwedits_,
			possearches, seedsearches, intrahits, interhits, ooms,
			sr,
			met, prm);
}

// MultiSeedAligner equivalent of SeedAligner::searchAllSeeds
void MultiSeedAligner::searchAllSeeds(
	std::vector< SeedAligner* > &palv,             // seed aligners
	const std::vector< EList<Seed>* > &pseedsv,    // search seeds
	const Ebwt* ebwtFw,                            // BWT index
	const Ebwt* ebwtBw,                            // BWT' index
	const std::vector< Read* > &preadv,            // read to align
	const Scoring& pens,                           // scoring scheme
	std::vector< AlignmentCacheIface* > &pcachev,  // local cache for seed alignments
	std::vector< SeedResults* > &psrv,             // holds all the seed hits
	std::vector< SeedSearchMetrics* > &pmetv,      // metrics
	std::vector< PerReadMetrics* > &pprmv)         // per-read metrics
{
	assert_eq(palv.size(), pseedsv.size())
	assert_eq(pseedsv.size(), preadv.size());
	assert_eq(preadv.size(), pcachev.size());
	assert_eq(pcachev.size(), psrv.size());
	assert_eq(psrv.size(), pmetv.size());
	assert_eq(pmetv.size(), pprmv.size());
	assert(ebwtFw != NULL);
	assert(ebwtFw->isInMemory());
	const size_t nels = palv.size();

	size_t max_numOffs = 0;
	for (size_t n=0; n<nels; n++) {
		assert(!pseedsv[n]->empty());
		assert(psrv[n]->repOk(&pcachev[n]->current()));
		palv[n]->setRead(ebwtFw, ebwtBw, *(preadv[n]), pens, *(pcachev[n]));
		size_t numOffs = psrv[n]->numOffs();
		if(numOffs>max_numOffs) max_numOffs=numOffs;
	}
	std::vector<uint64_t> possearches(nels,0);
	std::vector<uint64_t> seedsearches(nels,0);
	std::vector<uint64_t> intrahits(nels,0);
	std::vector<uint64_t> interhits(nels,0);
	std::vector<uint64_t> ooms(nels,0);

	// QVal is small, better to just pre-allocate max number
	std::vector< QVal > qvv(nels);

	// For each instantiated seed
	for(size_t i = 0; i < max_numOffs; i++) {
		std::vector< size_t > iter_idxs;
		iter_idxs.reserve(nels);
		for(size_t n=0; n<nels; n++) {
			if(i<psrv[n]->numOffs()) {
				iter_idxs.push_back(n);
			}
		}
		const size_t iter_nels = iter_idxs.size();
		for(int fwi = 0; fwi < 2; fwi++) {
			std::vector< size_t > r0_idxs;
			r0_idxs.reserve(iter_nels);

			const bool fw = (fwi == 0);
			size_t max_iss_size = 0;
			for(size_t ni=0; ni<iter_nels; ni++) {
				size_t n = iter_idxs[ni];
				assert(psrv[n]->repOk(&pcache[n]->current()));
				EList<InstantiatedSeed>& iss = psrv[n]->instantiatedSeeds(fw, i);
				if(iss.empty()) {
					// Cache hit in an across-read cache
					continue;
				}
				qvv[n].reset();
				int ret = palv[n]->setSeedResult(*(psrv[n]), qvv[n], fw, i);
				if(ret == -1) {
					// Out of memory when we tried to add key to map
					ooms[n]++;
					continue;
				}
				if(ret == 0) {
					// Not already in cache
					assert(pcachev[n]->aligning());
					possearches[n]++;
					r0_idxs.push_back(n);
					if (iss.size()>max_iss_size) max_iss_size=iss.size();
				} else {
					// Already in cache
					assert_eq(1, ret);
					assert(qvv[n].valid());
					intrahits[n]++;
					assert(!pcachev[n]->aligning());
					psrv[n]->add(
						qvv[n],    // range of ranges in cache
						pcachev[n]->current(), // cache
						i,     // seed index (from 5' end)
						fw);   // whether seed is from forward read
				}
			}

			const size_t r0_nels = r0_idxs.size();
			bool abort = false;
			for(size_t j = 0; j < max_iss_size; j++) {
				std::vector< size_t > j_idxs;
				j_idxs.reserve(r0_nels);
				for(size_t ni=0; ni<r0_nels; ni++) {
					size_t n = r0_idxs[ni];
					EList<InstantiatedSeed>& iss = psrv[n]->instantiatedSeeds(fw, i);

					if(j>=iss.size()) continue;

					// Set seq_ and qual_ appropriately, using the seed sequences
					// and qualities already installed in SeedResults
					assert_eq(fw, iss[j].fw);
					assert_eq(i, (int)iss[j].seedoffidx);
					palv[n]->s_ = &iss[j];
					j_idxs.push_back(n);
				}
				const size_t j_nels = j_idxs.size();
				std::vector< SeedAligner* > j_palv;
				j_palv.reserve(j_nels);
				for(size_t ni=0; ni<j_nels; ni++) {
					size_t n = j_idxs[ni];
					j_palv.push_back(palv[n]);
				}

				// Do the search with respect to seq_, qual_ and s_.
				if(!searchSeedBi(j_palv)) {
					// Memory exhausted during search
					for(size_t ni=0; ni<j_nels; ni++) {
						size_t n = j_idxs[ni];
						ooms[n]++;
					}
					abort = true;
					break;
				}
				for(size_t ni=0; ni<j_nels; ni++) {
					size_t n = j_idxs[ni];
					seedsearches[n]++;
					assert(pcachev[n].aligning());
				}
			}
			for(size_t ni=0; ni<r0_nels; ni++) {
				size_t n = r0_idxs[ni];

				if(!abort) {
					qvv[n] = pcachev[n]->finishAlign();
				}
				assert(abort || !pcachev[n]->aligning());
				if(qvv[n].valid()) {
					psrv[n]->add(
						qvv[n],    // range of ranges in cache
						pcachev[n]->current(), // cache
						i,     // seed index (from 5' end)
						fw);   // whether seed is from forward read
				}
			}
		} // for fwi
	} // for i
	for (size_t n=0; n<nels; n++) {
		finalizeOneAllSeeds(
			palv[n]->bwops_, palv[n]->bwedits_,
			possearches[n], seedsearches[n], intrahits[n], interhits[n], ooms[n],
			*(psrv[n]),
			*(pmetv[n]), *(pprmv[n]));
	}
}


bool SeedAligner::sanityPartial(
	const Ebwt*        ebwtFw, // BWT index
	const Ebwt*        ebwtBw, // BWT' index
	const BTDnaString& seq,
	size_t dep,
	size_t len,
	bool do1mm,
	TIndexOffU topfw,
	TIndexOffU botfw,
	TIndexOffU topbw,
	TIndexOffU botbw)
{
	tmpdnastr_.clear();
	for(size_t i = dep; i < len; i++) {
		tmpdnastr_.append(seq[i]);
	}
	TIndexOffU top_fw = 0, bot_fw = 0;
	ebwtFw->contains(tmpdnastr_, &top_fw, &bot_fw);
	assert_eq(top_fw, topfw);
	assert_eq(bot_fw, botfw);
	if(do1mm && ebwtBw != NULL) {
		tmpdnastr_.reverse();
		TIndexOffU top_bw = 0, bot_bw = 0;
		ebwtBw->contains(tmpdnastr_, &top_bw, &bot_bw);
		assert_eq(top_bw, topbw);
		assert_eq(bot_bw, botbw);
	}
	return true;
}

/**
 * Sweep right-to-left and left-to-right using exact matching.  Remember all
 * the SA ranges encountered along the way.  Report exact matches if there are
 * any.  Calculate a lower bound on the number of edits in an end-to-end
 * alignment.
 */
size_t SeedAligner::exactSweep(
	const Ebwt&        ebwt,    // BWT index
	const Read&        read,    // read to align
	const Scoring&     sc,      // scoring scheme
	bool               nofw,    // don't align forward read
	bool               norc,    // don't align revcomp read
	size_t             mineMax, // don't care about edit bounds > this
	size_t&            mineFw,  // minimum # edits for forward read
	size_t&            mineRc,  // minimum # edits for revcomp read
	bool               repex,   // report 0mm hits?
	SeedResults&       hits,    // holds all the seed hits (and exact hit)
	SeedSearchMetrics& met)     // metrics
{
	assert_gt(mineMax, 0);
	TIndexOffU top = 0, bot = 0;
	SideLocus tloc, bloc;
	const size_t len = read.length();
	size_t nelt = 0;
	for(int fwi = 0; fwi < 2; fwi++) {
		bool fw = (fwi == 0);
		if( fw && nofw) continue;
		if(!fw && norc) continue;
		const BTDnaString& seq = fw ? read.patFw : read.patRc;
		assert(!seq.empty());
		int ftabLen = ebwt.eh().ftabChars();
		size_t dep = 0;
		size_t nedit = 0;
		bool done = false;
		while(dep < len && !done) {
			top = bot = 0;
			size_t left = len - dep;
			assert_gt(left, 0);
			bool doFtab = ftabLen > 1 && left >= (size_t)ftabLen;
			if(doFtab) {
				// Does N interfere with use of Ftab?
				for(size_t i = 0; i < (size_t)ftabLen; i++) {
					int c = seq[len-dep-1-i];
					if(c > 3) {
						doFtab = false;
						break;
					}
				}
			}
			if(doFtab) {
				// Use ftab
				ebwt.ftabLoHi(seq, len - dep - ftabLen, false, top, bot);
				dep += (size_t)ftabLen;
			} else {
				// Use fchr
				int c = seq[len-dep-1];
				if(c < 4) {
					top = ebwt.fchr()[c];
					bot = ebwt.fchr()[c+1];
				}
				dep++;
			}
			if(bot <= top) {
				nedit++;
				if(nedit >= mineMax) {
					if(fw) { mineFw = nedit; } else { mineRc = nedit; }
					break;
				}
				continue;
			}
			INIT_LOCS(top, bot, tloc, bloc, ebwt);
			// Keep going
			while(dep < len) {
				int c = seq[len-dep-1];
				if(c > 3) {
					top = bot = 0;
				} else {
					if(bloc.valid()) {
						bwops_ += 2;
						top = ebwt.mapLF(tloc, c);
						bot = ebwt.mapLF(bloc, c);
					} else {
						bwops_++;
						top = ebwt.mapLF1(top, tloc, c);
						if(top == OFF_MASK) {
							top = bot = 0;
						} else {
							bot = top+1;
						}
					}
				}
				if(bot <= top) {
					nedit++;
					if(nedit >= mineMax) {
						if(fw) { mineFw = nedit; } else { mineRc = nedit; }
						done = true;
					}
					break;
				}
				INIT_LOCS(top, bot, tloc, bloc, ebwt);
				dep++;
			}
			if(done) {
				break;
			}
			if(dep == len) {
				// Set the minimum # edits
				if(fw) { mineFw = nedit; } else { mineRc = nedit; }
				// Done
				if(nedit == 0 && bot > top) {
					if(repex) {
						// This is an exact hit
						int64_t score = len * sc.match();
						if(fw) {
							hits.addExactEeFw(top, bot, NULL, NULL, fw, score);
							assert(ebwt.contains(seq, NULL, NULL));
						} else {
							hits.addExactEeRc(top, bot, NULL, NULL, fw, score);
							assert(ebwt.contains(seq, NULL, NULL));
						}
					}
					nelt += (bot - top);
				}
				break;
			}
			dep++;
		}
	}
	return nelt;
}

/**
 * Search for end-to-end exact hit for read.  Return true iff one is found.
 */
bool SeedAligner::oneMmSearch(
	const Ebwt*        ebwtFw, // BWT index
	const Ebwt*        ebwtBw, // BWT' index
	const Read&        read,   // read to align
	const Scoring&     sc,     // scoring
	int64_t            minsc,  // minimum score
	bool               nofw,   // don't align forward read
	bool               norc,   // don't align revcomp read
	bool               local,  // 1mm hits must be legal local alignments
	bool               repex,  // report 0mm hits?
	bool               rep1mm, // report 1mm hits?
	SeedResults&       hits,   // holds all the seed hits (and exact hit)
	SeedSearchMetrics& met)    // metrics
{
	assert(!rep1mm || ebwtBw != NULL);
	const size_t len = read.length();
	int nceil = sc.nCeil.f<int>((double)len);
	size_t ns = read.ns();
	if(ns > 1) {
		// Can't align this with <= 1 mismatches
		return false;
	} else if(ns == 1 && !rep1mm) {
		// Can't align this with 0 mismatches
		return false;
	}
	assert_geq(len, 2);
	assert(!rep1mm || ebwtBw->eh().ftabChars() == ebwtFw->eh().ftabChars());
#ifndef NDEBUG
	if(ebwtBw != NULL) {
		for(int i = 0; i < 4; i++) {
			assert_eq(ebwtBw->fchr()[i], ebwtFw->fchr()[i]);
		}
	}
#endif
	size_t halfFw = len >> 1;
	size_t halfBw = len >> 1;
	if((len & 1) != 0) {
		halfBw++;
	}
	assert_geq(halfFw, 1);
	assert_geq(halfBw, 1);
	SideLocus tloc, bloc;
	TIndexOffU t[4], b[4];   // dest BW ranges for BWT
	t[0] = t[1] = t[2] = t[3] = 0;
	b[0] = b[1] = b[2] = b[3] = 0;
	TIndexOffU tp[4], bp[4]; // dest BW ranges for BWT'
	tp[0] = tp[1] = tp[2] = tp[3] = 0;
	bp[0] = bp[1] = bp[2] = bp[3] = 0;
	TIndexOffU top = 0, bot = 0, topp = 0, botp = 0;
	// Align fw read / rc read
	bool results = false;
	for(int fwi = 0; fwi < 2; fwi++) {
		bool fw = (fwi == 0);
		if( fw && nofw) continue;
		if(!fw && norc) continue;
		// Align going right-to-left, left-to-right
		int lim = rep1mm ? 2 : 1;
		for(int ebwtfwi = 0; ebwtfwi < lim; ebwtfwi++) {
			bool ebwtfw = (ebwtfwi == 0);
			const Ebwt* ebwt  = (ebwtfw ? ebwtFw : ebwtBw);
			const Ebwt* ebwtp = (ebwtfw ? ebwtBw : ebwtFw);
			assert(rep1mm || ebwt->fw());
			const BTDnaString& seq =
				(fw ? (ebwtfw ? read.patFw : read.patFwRev) :
				      (ebwtfw ? read.patRc : read.patRcRev));
			assert(!seq.empty());
			const BTString& qual =
				(fw ? (ebwtfw ? read.qual    : read.qualRev) :
				      (ebwtfw ? read.qualRev : read.qual));
			int ftabLen = ebwt->eh().ftabChars();
			size_t nea = ebwtfw ? halfFw : halfBw;
			// Check if there's an N in the near portion
			bool skip = false;
			for(size_t dep = 0; dep < nea; dep++) {
				if(seq[len-dep-1] > 3) {
					skip = true;
					break;
				}
			}
			if(skip) {
				continue;
			}
			size_t dep = 0;
			// Align near half
			if(ftabLen > 1 && (size_t)ftabLen <= nea) {
				// Use ftab to jump partway into near half
				bool rev = !ebwtfw;
				ebwt->ftabLoHi(seq, len - ftabLen, rev, top, bot);
				if(rep1mm) {
					ebwtp->ftabLoHi(seq, len - ftabLen, rev, topp, botp);
					assert_eq(bot - top, botp - topp);
				}
				if(bot - top == 0) {
					continue;
				}
				int c = seq[len - ftabLen];
				t[c] = top; b[c] = bot;
				tp[c] = topp; bp[c] = botp;
				dep = ftabLen;
				// initialize tloc, bloc??
			} else {
				// Use fchr to jump in by 1 pos
				int c = seq[len-1];
				assert_range(0, 3, c);
				top = topp = tp[c] = ebwt->fchr()[c];
				bot = botp = bp[c] = ebwt->fchr()[c+1];
				if(bot - top == 0) {
					continue;
				}
				dep = 1;
				// initialize tloc, bloc??
			}
			INIT_LOCS(top, bot, tloc, bloc, *ebwt);
			assert(sanityPartial(ebwt, ebwtp, seq, len-dep, len, rep1mm, top, bot, topp, botp));
			bool do_continue = false;
			for(; dep < nea; dep++) {
				assert_lt(dep, len);
				int rdc = seq[len - dep - 1];
				tp[0] = tp[1] = tp[2] = tp[3] = topp;
				bp[0] = bp[1] = bp[2] = bp[3] = botp;
				if(bloc.valid()) {
					bwops_++;
					t[0] = t[1] = t[2] = t[3] = b[0] = b[1] = b[2] = b[3] = 0;
					ebwt->mapBiLFEx(tloc, bloc, t, b, tp, bp);
					SANITY_CHECK_4TUP(t, b, tp, bp);
					top = t[rdc]; bot = b[rdc];
					if(bot <= top) {
						do_continue = true;
						break;
					}
					topp = tp[rdc]; botp = bp[rdc];
					assert(!rep1mm || bot - top == botp - topp);
				} else {
					assert_eq(bot, top+1);
					assert(!rep1mm || botp == topp+1);
					bwops_++;
					top = ebwt->mapLF1(top, tloc, rdc);
					if(top == OFF_MASK) {
						do_continue = true;
						break;
					}
					bot = top + 1;
					t[rdc] = top; b[rdc] = bot;
					tp[rdc] = topp; bp[rdc] = botp;
					assert(!rep1mm || b[rdc] - t[rdc] == bp[rdc] - tp[rdc]);
					// topp/botp stay the same
				}
				INIT_LOCS(top, bot, tloc, bloc, *ebwt);
				assert(sanityPartial(ebwt, ebwtp, seq, len - dep - 1, len, rep1mm, top, bot, topp, botp));
			}
			if(do_continue) {
				continue;
			}
			// Align far half
			for(; dep < len; dep++) {
				int rdc = seq[len-dep-1];
				int quc = qual[len-dep-1];
				if(rdc > 3 && nceil == 0) {
					break;
				}
				tp[0] = tp[1] = tp[2] = tp[3] = topp;
				bp[0] = bp[1] = bp[2] = bp[3] = botp;
				int clo = 0, chi = 3;
				bool match = true;
				if(bloc.valid()) {
					bwops_++;
					t[0] = t[1] = t[2] = t[3] = b[0] = b[1] = b[2] = b[3] = 0;
					ebwt->mapBiLFEx(tloc, bloc, t, b, tp, bp);
					SANITY_CHECK_4TUP(t, b, tp, bp);
					match = rdc < 4;
					top = t[rdc]; bot = b[rdc];
					topp = tp[rdc]; botp = bp[rdc];
				} else {
					assert_eq(bot, top+1);
					assert(!rep1mm || botp == topp+1);
					bwops_++;
					clo = ebwt->mapLF1(top, tloc);
					match = (clo == rdc);
					assert_range(-1, 3, clo);
					if(clo < 0) {
						break; // Hit the $
					} else {
						t[clo] = top;
						b[clo] = bot = top + 1;
					}
					bp[clo] = botp;
					tp[clo] = topp;
					assert(!rep1mm || bot - top == botp - topp);
					assert(!rep1mm || b[clo] - t[clo] == bp[clo] - tp[clo]);
					chi = clo;
				}
				//assert(sanityPartial(ebwt, ebwtp, seq, len - dep - 1, len, rep1mm, top, bot, topp, botp));
				if(rep1mm && (ns == 0 || rdc > 3)) {
					for(int j = clo; j <= chi; j++) {
						if(j == rdc || b[j] == t[j]) {
							// Either matches read or isn't a possibility
							continue;
						}
						// Potential mismatch - next, try
						size_t depm = dep + 1;
						TIndexOffU topm = t[j], botm = b[j];
						TIndexOffU topmp = tp[j], botmp = bp[j];
						assert_eq(botm - topm, botmp - topmp);
						TIndexOffU tm[4], bm[4];   // dest BW ranges for BWT
						tm[0] = t[0]; tm[1] = t[1];
						tm[2] = t[2]; tm[3] = t[3];
						bm[0] = b[0]; bm[1] = t[1];
						bm[2] = b[2]; bm[3] = t[3];
						TIndexOffU tmp[4], bmp[4]; // dest BW ranges for BWT'
						tmp[0] = tp[0]; tmp[1] = tp[1];
						tmp[2] = tp[2]; tmp[3] = tp[3];
						bmp[0] = bp[0]; bmp[1] = tp[1];
						bmp[2] = bp[2]; bmp[3] = tp[3];
						SideLocus tlocm, blocm;
						INIT_LOCS(topm, botm, tlocm, blocm, *ebwt);
						for(; depm < len; depm++) {
							int rdcm = seq[len - depm - 1];
							tmp[0] = tmp[1] = tmp[2] = tmp[3] = topmp;
							bmp[0] = bmp[1] = bmp[2] = bmp[3] = botmp;
							if(blocm.valid()) {
								bwops_++;
								tm[0] = tm[1] = tm[2] = tm[3] =
								bm[0] = bm[1] = bm[2] = bm[3] = 0;
								ebwt->mapBiLFEx(tlocm, blocm, tm, bm, tmp, bmp);
								SANITY_CHECK_4TUP(tm, bm, tmp, bmp);
								topm = tm[rdcm]; botm = bm[rdcm];
								topmp = tmp[rdcm]; botmp = bmp[rdcm];
								if(botm <= topm) {
									break;
								}
							} else {
								assert_eq(botm, topm+1);
								assert_eq(botmp, topmp+1);
								bwops_++;
								topm = ebwt->mapLF1(topm, tlocm, rdcm);
								if(topm == OFF_MASK) {
									break;
								}
								botm = topm + 1;
								// topp/botp stay the same
							}
							INIT_LOCS(topm, botm, tlocm, blocm, *ebwt);
						}
						if(depm == len) {
							// Success; this is a 1MM hit
							size_t off5p = dep;  // offset from 5' end of read
							size_t offstr = dep; // offset into patFw/patRc
							if(fw == ebwtfw) {
								off5p = len - off5p - 1;
							}
							if(!ebwtfw) {
								offstr = len - offstr - 1;
							}
							Edit e((uint32_t)off5p, j, rdc, EDIT_TYPE_MM, false);
							results = true;
							int64_t score = (len - 1) * sc.match();
							// In --local mode, need to double-check that
							// end-to-end alignment doesn't violate  local
							// alignment principles.  Specifically, it
							// shouldn't to or below 0 anywhere in the middle.
							int pen = sc.score(rdc, (int)(1 << j), quc - 33);
							score += pen;
							bool valid = true;
							if(local) {
								int64_t locscore_fw = 0, locscore_bw = 0;
								for(size_t i = 0; i < len; i++) {
									if(i == dep) {
										if(locscore_fw + pen <= 0) {
											valid = false;
											break;
										}
										locscore_fw += pen;
									} else {
										locscore_fw += sc.match();
									}
									if(len-i-1 == dep) {
										if(locscore_bw + pen <= 0) {
											valid = false;
											break;
										}
										locscore_bw += pen;
									} else {
										locscore_bw += sc.match();
									}
								}
							}
							if(valid) {
								valid = score >= minsc;
							}
							if(valid) {
#ifndef NDEBUG
								BTDnaString& rf = tmprfdnastr_;
								rf.clear();
								edits_.clear();
								edits_.push_back(e);
								if(!fw) Edit::invertPoss(edits_, len, false);
								Edit::toRef(fw ? read.patFw : read.patRc, edits_, rf);
								if(!fw) Edit::invertPoss(edits_, len, false);
								assert_eq(len, rf.length());
								for(size_t i = 0; i < len; i++) {
									assert_lt((int)rf[i], 4);
								}
								ASSERT_ONLY(TIndexOffU toptmp = 0);
								ASSERT_ONLY(TIndexOffU bottmp = 0);
								assert(ebwtFw->contains(rf, &toptmp, &bottmp));
#endif
								TIndexOffU toprep = ebwtfw ? topm : topmp;
								TIndexOffU botrep = ebwtfw ? botm : botmp;
								assert_eq(toprep, toptmp);
								assert_eq(botrep, bottmp);
								hits.add1mmEe(toprep, botrep, &e, NULL, fw, score);
							}
						}
					}
				}
				if(bot > top && match) {
					assert_lt(rdc, 4);
					if(dep == len-1) {
						// Success; this is an exact hit
						if(ebwtfw && repex) {
							if(fw) {
								results = true;
								int64_t score = len * sc.match();
								hits.addExactEeFw(
									ebwtfw ? top : topp,
									ebwtfw ? bot : botp,
									NULL, NULL, fw, score);
								assert(ebwtFw->contains(seq, NULL, NULL));
							} else {
								results = true;
								int64_t score = len * sc.match();
								hits.addExactEeRc(
									ebwtfw ? top : topp,
									ebwtfw ? bot : botp,
									NULL, NULL, fw, score);
								assert(ebwtFw->contains(seq, NULL, NULL));
							}
						}
						break; // End of far loop
					} else {
						INIT_LOCS(top, bot, tloc, bloc, *ebwt);
						assert(sanityPartial(ebwt, ebwtp, seq, len - dep - 1, len, rep1mm, top, bot, topp, botp));
					}
				} else {
					break; // End of far loop
				}
			} // for(; dep < len; dep++)
		} // for(int ebwtfw = 0; ebwtfw < 2; ebwtfw++)
	} // for(int fw = 0; fw < 2; fw++)
	return results;
}

inline void
SeedAligner::prefetchNextLocsBi(
        TIndexOffU topf,              // top in BWT
        TIndexOffU botf,              // bot in BWT
        TIndexOffU topb,              // top in BWT'
        TIndexOffU botb,              // bot in BWT'
        int step                    // step to get ready for
        )
{
	if(step == (int)s_->steps.size()) return; // no more steps!
	// Which direction are we going in next?
	if(s_->steps[step] > 0) {
		// Left to right; use BWT'
		if(botb - topb == 1) {
			// Already down to 1 row; just init top locus
			SideLocus::prefetchFromRow(
				topb, ebwtBw_->eh(), ebwtBw_->ebwt());
		} else {
			SideLocus::prefetchFromTopBot(
				topb, botb, ebwtBw_->eh(), ebwtBw_->ebwt());
		}
	} else {
		// Right to left; use BWT
		if(botf - topf == 1) {
			// Already down to 1 row; just init top locus
			SideLocus::prefetchFromRow(
				topf, ebwtFw_->eh(), ebwtFw_->ebwt());
		} else {
			SideLocus::prefetchFromTopBot(
				topf, botf, ebwtFw_->eh(), ebwtFw_->ebwt());
		}
	}
}

/**
 * Get tloc, bloc ready for the next step.  If the new range is under
 * the ceiling.
 */
inline void
SeedAligner::nextLocsBi(
	SideLocus& tloc,            // top locus
	SideLocus& bloc,            // bot locus
	TIndexOffU topf,              // top in BWT
	TIndexOffU botf,              // bot in BWT
	TIndexOffU topb,              // top in BWT'
	TIndexOffU botb,              // bot in BWT'
	int step                    // step to get ready for
#if 0
	, const SABWOffTrack* prevOt, // previous tracker
	SABWOffTrack& ot            // current tracker
#endif
	)
{
	assert_gt(botf, 0);
	assert(ebwtBw_ == NULL || botb > 0);
	assert_geq(step, 0); // next step can't be first one
	assert(ebwtBw_ == NULL || botf-topf == botb-topb);
	if(step == (int)s_->steps.size()) return; // no more steps!
	// Which direction are we going in next?
	if(s_->steps[step] > 0) {
		// Left to right; use BWT'
		if(botb - topb == 1) {
			// Already down to 1 row; just init top locus
			tloc.initFromRow(topb, ebwtBw_->eh(), ebwtBw_->ebwt());
			bloc.invalidate();
		} else {
			SideLocus::initFromTopBot(
				topb, botb, ebwtBw_->eh(), ebwtBw_->ebwt(), tloc, bloc);
			assert(bloc.valid());
		}
	} else {
		// Right to left; use BWT
		if(botf - topf == 1) {
			// Already down to 1 row; just init top locus
			tloc.initFromRow(topf, ebwtFw_->eh(), ebwtFw_->ebwt());
			bloc.invalidate();
		} else {
			SideLocus::initFromTopBot(
				topf, botf, ebwtFw_->eh(), ebwtFw_->ebwt(), tloc, bloc);
			assert(bloc.valid());
		}
	}
	// Check if we should update the tracker with this refinement
#if 0
	if(botf-topf <= BW_OFF_TRACK_CEIL) {
		if(ot.size() == 0 && prevOt != NULL && prevOt->size() > 0) {
			// Inherit state from the predecessor
			ot = *prevOt;
		}
		bool ltr = s_->steps[step-1] > 0;
		int adj = abs(s_->steps[step-1])-1;
		const Ebwt* ebwt = ltr ? ebwtBw_ : ebwtFw_;
		ot.update(
			ltr ? topb : topf,    // top
			ltr ? botb : botf,    // bot
			adj,                  // adj (to be subtracted from offset)
			ebwt->offs(),         // offs array
			ebwt->eh().offRate(), // offrate (sample = every 1 << offrate elts)
			NULL                  // dead
		);
		assert_gt(ot.size(), 0);
	}
#endif
	assert(botf - topf == 1 ||  bloc.valid());
	assert(botf - topf > 1  || !bloc.valid());
}

/**
 * Report a seed hit found by searchSeedBi(), but first try to extend it out in
 * either direction as far as possible without hitting any edits.  This will
 * allow us to prioritize the seed hits better later on.  Call reportHit() when
 * we're done, which actually adds the hit to the cache.  Returns result from
 * calling reportHit().
 */
bool
SeedAligner::extendAndReportHit(
	TIndexOffU topf,                     // top in BWT
	TIndexOffU botf,                     // bot in BWT
	TIndexOffU topb,                     // top in BWT'
	TIndexOffU botb,                     // bot in BWT'
	uint16_t len,                      // length of hit
	DoublyLinkedList<Edit> *prevEdit)  // previous edit
{
	size_t nlex = 0, nrex = 0;
	TIndexOffU t[4], b[4];
	TIndexOffU tp[4], bp[4];
	SideLocus tloc, bloc;
	if(off_ > 0) {
		const Ebwt *ebwt = ebwtFw_;
		assert(ebwt != NULL);
		// Extend left using forward index
		const BTDnaString& seq = fw_ ? read_->patFw : read_->patRc;
		// See what we get by extending 
		TIndexOffU top = topf, bot = botf;
		t[0] = t[1] = t[2] = t[3] = 0;
		b[0] = b[1] = b[2] = b[3] = 0;
		tp[0] = tp[1] = tp[2] = tp[3] = topb;
		bp[0] = bp[1] = bp[2] = bp[3] = botb;
		SideLocus tloc, bloc;
		INIT_LOCS(top, bot, tloc, bloc, *ebwt);
		for(size_t ii = off_; ii > 0; ii--) {
			size_t i = ii-1;
			// Get char from read
			int rdc = seq.get(i);
			// See what we get by extending 
			if(bloc.valid()) {
				bwops_++;
				t[0] = t[1] = t[2] = t[3] =
				b[0] = b[1] = b[2] = b[3] = 0;
				ebwt->mapBiLFEx(tloc, bloc, t, b, tp, bp);
				SANITY_CHECK_4TUP(t, b, tp, bp);
				int nonz = -1;
				bool abort = false;
				for(int j = 0; j < 4; j++) {
					if(b[i] > t[i]) {
						if(nonz >= 0) {
							abort = true;
							break;
						}
						nonz = j;
						top = t[i]; bot = b[i];
					}
				}
				if(abort || nonz != rdc) {
					break;
				}
			} else {
				assert_eq(bot, top+1);
				bwops_++;
				int c = ebwt->mapLF1(top, tloc);
				if(c != rdc) {
					break;
				}
				bot = top + 1;
			}
			if(++nlex == 255) {
				break;
			}
			INIT_LOCS(top, bot, tloc, bloc, *ebwt);
		}
	}
	size_t rdlen = read_->length();
	size_t nright = rdlen - off_ - len;
	if(nright > 0 && ebwtBw_ != NULL) {
		const Ebwt *ebwt = ebwtBw_;
		assert(ebwt != NULL);
		// Extend right using backward index
		const BTDnaString& seq = fw_ ? read_->patFw : read_->patRc;
		// See what we get by extending 
		TIndexOffU top = topb, bot = botb;
		t[0] = t[1] = t[2] = t[3] = 0;
		b[0] = b[1] = b[2] = b[3] = 0;
		tp[0] = tp[1] = tp[2] = tp[3] = topb;
		bp[0] = bp[1] = bp[2] = bp[3] = botb;
		INIT_LOCS(top, bot, tloc, bloc, *ebwt);
		for(size_t i = off_ + len; i < rdlen; i++) {
			// Get char from read
			int rdc = seq.get(i);
			// See what we get by extending 
			if(bloc.valid()) {
				bwops_++;
				t[0] = t[1] = t[2] = t[3] =
				b[0] = b[1] = b[2] = b[3] = 0;
				ebwt->mapBiLFEx(tloc, bloc, t, b, tp, bp);
				SANITY_CHECK_4TUP(t, b, tp, bp);
				int nonz = -1;
				bool abort = false;
				for(int j = 0; j < 4; j++) {
					if(b[i] > t[i]) {
						if(nonz >= 0) {
							abort = true;
							break;
						}
						nonz = j;
						top = t[i]; bot = b[i];
					}
				}
				if(abort || nonz != rdc) {
					break;
				}
			} else {
				assert_eq(bot, top+1);
				bwops_++;
				int c = ebwt->mapLF1(top, tloc);
				if(c != rdc) {
					break;
				}
				bot = top + 1;
			}
			if(++nrex == 255) {
				break;
			}
			INIT_LOCS(top, bot, tloc, bloc, *ebwt);
		}
	}
	assert_lt(nlex, rdlen);
	assert_leq(nlex, off_);
	assert_lt(nrex, rdlen);
	return reportHit(topf, botf, topb, botb, len, prevEdit);
}

/**
 * Report a seed hit found by searchSeedBi() by adding it to the cache.  Return
 * false if the hit could not be reported because of, e.g., cache exhaustion.
 */
bool
SeedAligner::reportHit(
	TIndexOffU topf,                     // top in BWT
	TIndexOffU botf,                     // bot in BWT
	TIndexOffU topb,                     // top in BWT'
	TIndexOffU botb,                     // bot in BWT'
	uint16_t len,                      // length of hit
	DoublyLinkedList<Edit> *prevEdit)  // previous edit
{
	// Add information about the seed hit to AlignmentCache.  This
	// information eventually makes its way back to the SeedResults
	// object when we call finishAlign(...).
	BTDnaString& rf = tmprfdnastr_;
	rf.clear();
	edits_.clear();
	if(prevEdit != NULL) {
		prevEdit->toList(edits_);
		Edit::sort(edits_);
		assert(Edit::repOk(edits_, *seq_));
		Edit::toRef(*seq_, edits_, rf);
	} else {
		rf = *seq_;
	}
	// Sanity check: shouldn't add the same hit twice.  If this
	// happens, it may be because our zone Constraints are not set up
	// properly and erroneously return true from acceptable() when they
	// should return false in some cases.
	assert_eq(hits_.size(), ca_->curNumRanges());
	assert(hits_.insert(rf));
	if(!ca_->addOnTheFly(rf, topf, botf, topb, botb)) {
		return false;
	}
	assert_eq(hits_.size(), ca_->curNumRanges());
#ifndef NDEBUG
	// Sanity check that the topf/botf and topb/botb ranges really
	// correspond to the reference sequence aligned to
	{
		BTDnaString rfr;
		TIndexOffU tpf, btf, tpb, btb;
		tpf = btf = tpb = btb = 0;
		assert(ebwtFw_->contains(rf, &tpf, &btf));
		if(ebwtBw_ != NULL) {
			rfr = rf;
			rfr.reverse();
			assert(ebwtBw_->contains(rfr, &tpb, &btb));
			assert_eq(tpf, topf);
			assert_eq(btf, botf);
			assert_eq(tpb, topb);
			assert_eq(btb, botb);
		}
	}
#endif
	return true;
}

class SeedAlignerSearchParams {
public:
	int step;
	BwtTopBot bwt;         // The 4 BWT idxs
	SideLocus tloc;       // locus for top (perhaps unititialized)
	SideLocus bloc;       // locus for bot (perhaps unititialized)
	std::array<Constraint,3> cv;        // constraints to enforce in seed zones
	Constraint overall;   // overall constraints to enforce
	DoublyLinkedList<Edit> *prevEdit;  // previous edit

	SeedAlignerSearchParams(
		const int _step,              // depth into steps_[] array
		const BwtTopBot &_bwt,        // The 4 BWT idxs
		const SideLocus &_tloc,       // locus for top (perhaps unititialized)
		const SideLocus &_bloc,       // locus for bot (perhaps unititialized)
		const std::array<Constraint,3> _cv,        // constraints to enforce in seed zones
		const Constraint &_overall,   // overall constraints to enforce
		DoublyLinkedList<Edit> *_prevEdit)  // previous edit
	: step(_step)
	, bwt(_bwt)
	, tloc(_tloc)
	, bloc(_bloc)
	, cv(_cv)
	, overall(_overall)
	, prevEdit(_prevEdit)
	{}

	SeedAlignerSearchParams(
		const int _step,              // depth into steps_[] array
		const BwtTopBot &_bwt,        // The 4 BWT idxs
		const SideLocus &_tloc,       // locus for top (perhaps unititialized)
		const SideLocus &_bloc,       // locus for bot (perhaps unititialized)
		const Constraint &_c0,        // constraints to enforce in seed zone 0
		const Constraint &_c1,        // constraints to enforce in seed zone 1
		const Constraint &_c2,        // constraints to enforce in seed zone 2
		const Constraint &_overall,   // overall constraints to enforce
		DoublyLinkedList<Edit> *_prevEdit)  // previous edit
	: step(_step)
	, bwt(_bwt)
	, tloc(_tloc)
	, bloc(_bloc)
	, cv{ _c0, _c1, _c2 }
	, overall(_overall)
	, prevEdit(_prevEdit)
	{}

	// create an empty bwt, tloc and bloc, with step=0
	SeedAlignerSearchParams(
		const Constraint &_c0,        // constraints to enforce in seed zone 0
		const Constraint &_c1,        // constraints to enforce in seed zone 1
		const Constraint &_c2,        // constraints to enforce in seed zone 2
		const Constraint &_overall,   // overall constraints to enforce
		DoublyLinkedList<Edit> *_prevEdit)  // previous edit
	: step(0)
	, bwt()
	, tloc()
	, bloc()
	, cv{ _c0, _c1, _c2 }
	, overall(_overall)
	, prevEdit(_prevEdit)
	{}

	void checkCV() const {
			assert(cv[0].acceptable());
			assert(cv[1].acceptable());
			assert(cv[2].acceptable());
	}

};

// return true, if we are already done
bool
SeedAligner::startSearchSeedBi(
	int depth,            // recursion depth
	SeedAlignerSearchParams &p,
	bool &oom             // did we run out of memory?
	)
{
	assert(s_ != NULL);
	const InstantiatedSeed& s = *s_;
	assert_gt(s.steps.size(), 0);
	assert(ebwtBw_ == NULL || ebwtBw_->eh().ftabChars() == ebwtFw_->eh().ftabChars());
#ifndef NDEBUG
	for(int i = 0; i < 4; i++) {
		assert(ebwtBw_ == NULL || ebwtBw_->fchr()[i] == ebwtFw_->fchr()[i]);
	}
#endif
	oom = false;
	if(p.step == (int)s.steps.size()) {
		// Finished aligning seed
		p.checkCV();
		if(!reportHit(p.bwt, seq_->length(), p.prevEdit)) {
			oom = true; // Memory exhausted
		}
		return true;
	}
#ifndef NDEBUG
	if(depth > 0) {
		assert(p.bwt.botf - p.bwt.topf == 1 ||  p.bloc.valid());
		assert(p.bwt.botf - p.bwt.topf > 1  || !p.bloc.valid());
	}
#endif
	if(p.step == 0) {
		// Just starting
		assert(p.prevEdit == NULL);
		assert(!p.tloc.valid());
		assert(!p.bloc.valid());
		int off = s.steps[0];
		bool ltr = off > 0;
		off = abs(off)-1;
		// Check whether/how far we can jump using ftab or fchr
		int ftabLen = ebwtFw_->eh().ftabChars();
		if(ftabLen > 1 && ftabLen <= s.maxjump) {
			if(!ltr) {
				assert_geq(off+1, ftabLen-1);
				off = off - ftabLen + 1;
			}
			ebwtFw_->ftabLoHi(*seq_, off, false, p.bwt.topf, p.bwt.botf);
			#ifdef NDEBUG
			if(p.bwt.botf - p.bwt.topf == 0) return true;
			#endif
			#ifdef NDEBUG
			if(ebwtBw_ != NULL) {
				p.bwt.topb = ebwtBw_->ftabHi(*seq_, off);
				p.bwt.botb = p.bwt.topb + (p.bwt.botf-p.bwt.topf);
			}
			#else
			if(ebwtBw_ != NULL) {
				ebwtBw_->ftabLoHi(*seq_, off, false, p.bwt.topb, p.bwt.botb);
				assert_eq(p.bwt.botf-p.bwt.topf, p.bwt.botb-p.bwt.topb);
			}
			if(p.bwt.botf - p.bwt.topf == 0) return true;
			#endif
			p.step += ftabLen;
		} else if(s.maxjump > 0) {
			// Use fchr
			int c = (*seq_)[off];
			assert_range(0, 3, c);
			p.bwt.topf = p.bwt.topb = ebwtFw_->fchr()[c];
			p.bwt.botf = p.bwt.botb = ebwtFw_->fchr()[c+1];
			if(p.bwt.botf - p.bwt.topf == 0) return true;
			p.step++;
		} else {
			assert_eq(0, s.maxjump);
			p.bwt.topf = p.bwt.topb = 0;
			p.bwt.botf = p.bwt.botb = ebwtFw_->fchr()[4];
		}
		if(p.step == (int)s.steps.size()) {
			// Finished aligning seed
			p.checkCV();
			if(!reportHit(p.bwt, seq_->length(), p.prevEdit)) {
				oom = true; // Memory exhausted
			}
			return true;
		}
		nextLocsBi(p.tloc, p.bloc, p.bwt, p.step);
		assert(p.tloc.valid());
	} else assert(p.prevEdit != NULL);
	assert(p.tloc.valid());
	assert(p.bwt.botf - p.bwt.topf == 1 ||  p.bloc.valid());
	assert(p.bwt.botf - p.bwt.topf > 1  || !p.bloc.valid());
	assert_geq(p.step, 0);

	return false;
}

class SeedAlignerSearchState {
public:
	TIndexOffU tp[4], bp[4]; // dest BW ranges for "prime" index
	TIndexOffU t[4], b[4];   // dest BW ranges
	TIndexOffU *tf, *tb, *bf, *bb; // depend on ltr
	const Ebwt* ebwt;

	TIndexOffU ntop;
	int off;
	bool ltr;
public:
	void setOff(
		size_t _off,
		const BwtTopBot &bwt,      // The 4 BWT idxs
		const Ebwt* ebwtFw_,       // forward index (BWT)
		const Ebwt* ebwtBw_)       // backward/mirror index (BWT')
	{
		off = _off;
		ltr = off > 0;
		t[0] = t[1] = t[2] = t[3] = b[0] = b[1] = b[2] = b[3] = 0;
		off = abs(off)-1;
		if(ltr) {
			ebwt = ebwtBw_;
			tp[0] = tp[1] = tp[2] = tp[3] = bwt.topf;
			bp[0] = bp[1] = bp[2] = bp[3] = bwt.botf;
			tf = tp; tb = t;
			bf = bp; bb = b;
			ntop = bwt.topb;
		} else {
			ebwt = ebwtFw_;
			tp[0] = tp[1] = tp[2] = tp[3] = bwt.topb;
			bp[0] = bp[1] = bp[2] = bp[3] = bwt.botb;
			tf = t; tb = tp;
			bf = b; bb = bp;
			ntop = bwt.topf;
		}
		assert(ebwt != NULL);
	}

public:
#ifndef NDEBUG
	TIndexOffU lasttot;

	void initLastTot(TIndexOffU tot) { lasttot = tot;}
	void assertLeqAndSetLastTot(TIndexOffU tot) {
		assert_leq(tot, lasttot);
		lasttot = tot;
	}
#else
	// noop in production code
	void initLastTot(TIndexOffU tot) {}
	void assertLeqAndSetLastTot(TIndexOffU tot) {};
#endif

};

class SeedAlignerSearchSave {
public:
	SeedAlignerSearchSave(
		Constraint &cons, Constraint &ovCons,
		SideLocus &tloc, SideLocus &bloc)
	: orgCons(cons), orgOvCons(ovCons)
	, orgTloc(tloc), orgBloc(bloc)
	, oldCons(cons), oldOvCons(ovCons)
	, oldTloc(tloc), oldBloc(bloc)
	{}

	~SeedAlignerSearchSave() {
		orgCons = oldCons; orgOvCons = oldOvCons;
		orgTloc = oldTloc; orgBloc = oldBloc;
	}

private:
	// reference to the original variables
	Constraint &orgCons;
	Constraint &orgOvCons;
	SideLocus &orgTloc;
	SideLocus &orgBloc;

	// copy of the originals
	const Constraint oldCons;
	const Constraint oldOvCons;
	const SideLocus oldTloc;
	const SideLocus oldBloc;
};

// State used for recursion
class SeedAlignerSearchRecState {
public:
	SeedAlignerSearchRecState(
			int j,
			int c,
			const SeedAlignerSearchState &sstate,
			DoublyLinkedList<Edit> *prevEdit)
	: bwt(sstate.tf[j], sstate.bf[j], sstate.tb[j], sstate.bb[j])
	, edit(sstate.off, j, c, EDIT_TYPE_MM, false)
	, editl()
	, _prevEdit(prevEdit)
	{
		assert(_prevEdit == NULL || _prevEdit->next == NULL);
		editl.payload = edit;
		if(_prevEdit != NULL) {
			_prevEdit->next = &editl;
			editl.prev = _prevEdit;
		}
		assert(editl.next == NULL);
	}

	~SeedAlignerSearchRecState() {
		if(_prevEdit != NULL) _prevEdit->next = NULL;
	}

	BwtTopBot bwt;
	Edit edit;
	DoublyLinkedList<Edit> editl;
private:
	DoublyLinkedList<Edit> *_prevEdit;
};

class MultiSeedAlignerSearchParams : public SeedAlignerSearchParams {
public:
	SeedAligner& al;


	MultiSeedAlignerSearchParams(
		SeedAligner& _al,             // SeedAligner object associated with params
		const int _step,              // depth into steps_[] array
		const BwtTopBot &_bwt,        // The 4 BWT idxs
		const SideLocus &_tloc,       // locus for top (perhaps unititialized)
		const SideLocus &_bloc,       // locus for bot (perhaps unititialized)
		const std::array<Constraint,3> _cv,        // constraints to enforce in seed zones
		const Constraint &_overall,   // overall constraints to enforce
		DoublyLinkedList<Edit> *_prevEdit)  // previous edit
	: SeedAlignerSearchParams(_step, _bwt, _tloc, _bloc, _cv, _overall, _prevEdit)
	, al(_al)
	{}

	MultiSeedAlignerSearchParams(
		SeedAligner& _al,             // SeedAligner object associated with params
		const int _step,              // depth into steps_[] array
		const BwtTopBot &_bwt,        // The 4 BWT idxs
		const SideLocus &_tloc,       // locus for top (perhaps unititialized)
		const SideLocus &_bloc,       // locus for bot (perhaps unititialized)
		const Constraint &_c0,        // constraints to enforce in seed zone 0
		const Constraint &_c1,        // constraints to enforce in seed zone 1
		const Constraint &_c2,        // constraints to enforce in seed zone 2
		const Constraint &_overall,   // overall constraints to enforce
		DoublyLinkedList<Edit> *_prevEdit)  // previous edit
	: SeedAlignerSearchParams(_step, _bwt, _tloc, _bloc, _c0, _c1, _c2, _overall, _prevEdit)
	, al(_al)
	{}

	// create an empty bwt, tloc and bloc
	MultiSeedAlignerSearchParams(
		SeedAligner& _al,             // SeedAligner object associated with params
		const Constraint &_c0,        // constraints to enforce in seed zone 0
		const Constraint &_c1,        // constraints to enforce in seed zone 1
		const Constraint &_c2,        // constraints to enforce in seed zone 2
		const Constraint &_overall,   // overall constraints to enforce
		DoublyLinkedList<Edit> *_prevEdit)  // previous edit
	: SeedAlignerSearchParams(_c0, _c1, _c2, _overall, _prevEdit)
	, al(_al)
	{}
};

class MultiSeedAlignerSearchState : public SeedAlignerSearchState {
public:
	bool bail;
	bool leaveZone;
	int c;
	Constraint* pcons;
};

/**
 * Given a seed, search.  Assumes zone 0 = no backtracking.
 *
 * Return a list of Seed hits.
 * 1. Edits
 * 2. Bidirectional BWT range(s) on either end
 */

bool
MultiSeedAligner::searchSeedBi(
	int depth,            // recursion depth
	std::vector< MultiSeedAlignerSearchParams* > &ppv)
{
	const size_t nels = ppv.size();

	bool done = true;

	for(auto ppel : ppv) {
		bool my_oom = false;
		bool my_done = ppel->al.startSearchSeedBi(
				depth,
				*ppel,
				my_oom);
		if(my_done) {
			if (my_oom) {
				return false;
			}
			// only >=0 steps are valid, so make it invalid
			ppel->step = -1;
		}
		done &= my_done;
	}
	if(done) {
		return true;
	}

	std::vector<MultiSeedAlignerSearchState> sstatev(nels);
	size_t min_step=INT_MAX;
	size_t max_step=0;

	for (size_t n=0; n<nels; n++) {
		MultiSeedAlignerSearchParams &p = *(ppv[n]);
		if (p.step<0) continue; // invalid => done
		if((size_t)p.step<min_step) min_step=p.step;

		const size_t ssteps = p.al.s_->steps.size();
		if(ssteps>max_step) max_step=ssteps;

		sstatev[n].initLastTot(p.bwt.botf - p.bwt.topf);
	}
	for(size_t i = min_step; i < max_step; i++) {
	   for (size_t n=0; n<nels; n++) { // iterating OK, could be parallel, too
		MultiSeedAlignerSearchParams &p = *(ppv[n]);
		const InstantiatedSeed& s = *p.al.s_;
		if((p.step<(int)i)||(i>=s.steps.size())) continue;
		SeedAlignerSearchState& sstate = sstatev[n];

		assert_gt(p.bwt.botf, p.bwt.topf);
		assert(p.bwt.botf - p.bwt.topf == 1 ||  p.bloc.valid());
		assert(p.bwt.botf - p.bwt.topf > 1  || !p.bloc.valid());
		assert(p.al.ebwtBw_ == NULL || p.bwt.botf-p.bwt.topf == p.bwt.botb-p.bwt.topb);
		assert(p.tloc.valid());
		sstate.setOff(p.al.s_->steps[i], p.bwt, p.al.ebwtFw_, p.al.ebwtBw_);
		__builtin_prefetch(&((*p.al.seq_)[sstate.off]));
		__builtin_prefetch(&((*p.al.qual_)[sstate.off]));
	   }

	   for (size_t n=0; n<nels; n++) { // iterating OK, could be parallel, too
		MultiSeedAlignerSearchParams &p = *(ppv[n]);
		const InstantiatedSeed& s = *p.al.s_;
		if((p.step<(int)i)||(i>=s.steps.size())) continue;
		MultiSeedAlignerSearchState& sstate = sstatev[n];

		if(p.bloc.valid()) {
			// Range delimited by tloc/bloc has size >1.  If size == 1,
			// we use a simpler query (see if(!bloc.valid()) blocks below)
			p.al.bwops_++;
			sstate.ebwt->mapBiLFEx(p.tloc, p.bloc, sstate.t, sstate.b, sstate.tp, sstate.bp);
			ASSERT_ONLY(TIndexOffU tot = (sstate.b[0]-sstate.t[0])+(sstate.b[1]-sstate.t[1])+(sstate.b[2]-sstate.t[2])+(sstate.b[3]-sstate.t[3]));
			ASSERT_ONLY(TIndexOffU totp = (sstate.bp[0]-sstate.tp[0])+(sstate.bp[1]-sstate.tp[1])+(sstate.bp[2]-sstate.tp[2])+(sstate.bp[3]-sstate.tp[3]));
			assert_eq(tot, totp);
#ifndef NDEBUG
			sstate.assertLeqAndSetLastTot(tot);
#endif
		}

		int c = (*p.al.seq_)[sstate.off];  assert_range(0, 4, c);
		sstate.c = c;

		sstate.leaveZone = s.zones[i].first < 0;

		Constraint& cons    = p.cv[abs(s.zones[i].first)];
		sstate.pcons = &cons;

		sstate.bail = true;
		//Constraint& insCons = p.cv[abs(s.zones[i].second)];
		// Is it legal for us to advance on characters other than 'c'?
		if(!(cons.mustMatch() && !p.overall.mustMatch()) || c == 4) {
			// There may be legal edits
			sstate.bail = false;
			if(!p.bloc.valid()) {
				// Range delimited by tloc/bloc has size 1
				p.al.bwops_++;
				int cc = sstate.ebwt->mapLF1(sstate.ntop, p.tloc);
				assert_range(-1, 3, cc);
				if(cc < 0) sstate.bail = true;
				else { sstate.t[cc] = sstate.ntop; sstate.b[cc] = sstate.ntop+1; }
			}
		}
	   } // for n
	   {
	     std::vector< std::shared_ptr<SeedAlignerSearchSave> > savev;
	     std::vector<size_t> recloop;
	     savev.reserve(nels); recloop.reserve(nels);
	     for (size_t n=0; n<nels; n++) { // first get recloop
		MultiSeedAlignerSearchParams &p = *(ppv[n]);
		const InstantiatedSeed& s = *p.al.s_;
		if((p.step<(int)i)||(i>=s.steps.size())) continue;
		MultiSeedAlignerSearchState& sstate = sstatev[n];
		if(!sstate.bail) {
				int c = sstate.c;

				Constraint& cons    = *sstate.pcons;

				int q = (*p.al.qual_)[sstate.off];
				if((cons.canMismatch(q, *p.al.sc_) && p.overall.canMismatch(q, *p.al.sc_)) || c == 4) {
					SeedAlignerSearchSave* psave = new SeedAlignerSearchSave(cons, p.overall, p.tloc, p.bloc);
					savev.push_back( std::shared_ptr<SeedAlignerSearchSave>(psave) );

					if(c != 4) {
						cons.chargeMismatch(q, *p.al.sc_);
						p.overall.chargeMismatch(q, *p.al.sc_);
					}
					// Can leave the zone as-is
					if(!sstate.leaveZone || (cons.acceptable() && p.overall.acceptable())) {
						recloop.push_back(n);
					} else {
						// Not enough edits to make this path
						// non-redundant with other seeds
					}
				}
			}
	     } // for n
	     const size_t nrlels = recloop.size();
	     for(int j = 0; j < 4; j++) { // must be executed in order
		std::vector< std::shared_ptr<SeedAlignerSearchRecState> > rstatev_save;
		std::vector< std::shared_ptr<MultiSeedAlignerSearchParams> > p2v_save;
		std::vector<MultiSeedAlignerSearchParams*> p2v;
		rstatev_save.reserve(nrlels);
		p2v_save.reserve(nrlels); p2v.reserve(nrlels);
		for (size_t ni=0; ni<nrlels; ni++) { // build p2v
			const size_t n=recloop[ni];
			MultiSeedAlignerSearchParams &p = *(ppv[n]);
			MultiSeedAlignerSearchState& sstate = sstatev[n];

			int c = sstate.c;
			if(j == c || sstate.b[j] == sstate.t[j]) continue;

			// Potential mismatch
			SeedAlignerSearchRecState* prstate = new SeedAlignerSearchRecState(j, c, sstate, p.prevEdit);
			rstatev_save.push_back(std::shared_ptr<SeedAlignerSearchRecState>(prstate));

			p.al.nextLocsBi(p.tloc, p.bloc, prstate->bwt, i+1);
			p.al.bwedits_++;
			MultiSeedAlignerSearchParams* p2 = new MultiSeedAlignerSearchParams(
				p.al,              // SeedAligner object associated with params
				i+1,               // depth into steps_[] array
				prstate->bwt,      // The 4 BWT idxs
				p.tloc,            // locus for top (perhaps unititialized)
				p.bloc,            // locus for bot (perhaps unititialized)
				p.cv,              // constraints to enforce in seed zones
				p.overall,         // overall constraints to enforce
				&prstate->editl);  // latest edit
			p2v.push_back(p2);
			p2v_save.push_back(std::shared_ptr<MultiSeedAlignerSearchParams>(p2) );
		} // for ni
		if(p2v.size()>0) {
			// recurse all of the p2v elements at once
			if(!searchSeedBi(
				depth+1, // recursion depth
				p2v))
			{
				// oom detected, just bail out all the nels
				return false;
			}
		}
		// as rstatev gets out of scope, p.prevEdit->next is updated
	     } // for j

	     // as savev gets out of scope,
             // restores cons, p.overall, p.tloc, p.bloc
	   } // end savev and recloop scope

	   for (size_t n=0; n<nels; n++) { // OK to iterate, could be parallel, too
		MultiSeedAlignerSearchParams &p = *(ppv[n]);
		const InstantiatedSeed& s = *p.al.s_;
		if((p.step<(int)i)||(i>=s.steps.size())) continue;
		MultiSeedAlignerSearchState& sstate = sstatev[n];
		Constraint& cons    = *sstate.pcons;

		if(!sstate.bail) {
				if(cons.canGap() && p.overall.canGap()) {
					throw 1; // TODO
//					int delEx = 0;
//					if(cons.canDelete(delEx, *sc_) && overall.canDelete(delEx, *sc_)) {
//						// Try delete
//					}
//					int insEx = 0;
//					if(insCons.canInsert(insEx, *sc_) && overall.canInsert(insEx, *sc_)) {
//						// Try insert
//					}
				}
		}

		int c = sstate.c;
		if(c == 4) {
			// couldn't handle the N
			p.step = -1; // invalidate, same as marking it done
			continue;
		}

		if(sstate.leaveZone && (!cons.acceptable() || !p.overall.acceptable())) {
			// Not enough edits to make this path non-redundant with
			// other seeds
			p.step = -1; // invalidate, same as marking it done
			continue;
		}
		if(!p.bloc.valid()) {
			assert(p.al.ebwtBw_ == NULL || sstate.bp[c] == sstate.tp[c]+1);
			// Range delimited by tloc/bloc has size 1
			p.al.bwops_++;
			sstate.t[c] = sstate.ebwt->mapLF1(sstate.ntop, p.tloc, c);
			if(sstate.t[c] == OFF_MASK) {
				p.step = -1; // invalidate, same as marking it done
				continue;
			}
			assert_geq(sstate.t[c], sstate.ebwt->fchr()[c]);
			assert_lt(sstate.t[c],  sstate.ebwt->fchr()[c+1]);
			sstate.b[c] = sstate.t[c]+1;
			assert_gt(sstate.b[c], 0);
		}
		assert(p.al.ebwtBw_ == NULL || sstate.bf[c]-sstate.tf[c] == sstate.bb[c]-sstate.tb[c]);
		sstate.assertLeqAndSetLastTot(sstate.bf[c]-sstate.tf[c]);
		if(sstate.b[c] == sstate.t[c]) {
			p.step = -1; // invalidate, same as marking it done
			continue;
		}
		p.bwt.set(sstate.tf[c], sstate.bf[c], sstate.tb[c], sstate.bb[c]);
		if(i+1 == s.steps.size()) {
			// Finished aligning seed
			p.checkCV();
			if(!p.al.reportHit(p.bwt, p.al.seq_->length(), p.prevEdit)) {
				return false; // Memory exhausted
			}
			p.step = -1; // invalidate, same as marking it done
			continue;
		}
		p.al.nextLocsBi(p.tloc, p.bloc, p.bwt, i+1);
	   } // for n
	} // for i
	return true;
}

/**
 * Wrapper for initial invcation of searchSeed.
 */
bool
SeedAligner::searchSeedBi() {
	MultiSeedAlignerSearchParams p(*this, s_->cons[0], s_->cons[1], s_->cons[2], s_->overall, NULL);
	std::vector< MultiSeedAlignerSearchParams* > ppv(1, &p);
	return MultiSeedAligner::searchSeedBi(0, ppv);
}

// MultiSeedAligner version of the initial invocation
bool
MultiSeedAligner::searchSeedBi(std::vector< SeedAligner* > &palv) {
	// use a self-cleaning version for ease of use
	std::vector< std::shared_ptr<MultiSeedAlignerSearchParams> > ppv_save;
	// but pass the unmanaged vector to the outside
	std::vector< MultiSeedAlignerSearchParams* > ppv;
	ppv_save.reserve(palv.size()); ppv.reserve(palv.size());
	for(auto pal : palv) {
		const InstantiatedSeed* s = pal->s_;
		MultiSeedAlignerSearchParams* p = new MultiSeedAlignerSearchParams(*pal, s->cons[0], s->cons[1], s->cons[2], s->overall, NULL);
		ppv.push_back( p );
		ppv_save.push_back( std::shared_ptr<MultiSeedAlignerSearchParams>( p ) );
	}
	return searchSeedBi(0, ppv);
}


#ifdef ALIGNER_SEED_MAIN

#include <getopt.h>
#include <string>

/**
 * Parse an int out of optarg and enforce that it be at least 'lower';
 * if it is less than 'lower', than output the given error message and
 * exit with an error and a usage message.
 */
static int parseInt(const char *errmsg, const char *arg) {
	long l;
	char *endPtr = NULL;
	l = strtol(arg, &endPtr, 10);
	if (endPtr != NULL) {
		return (int32_t)l;
	}
	cerr << errmsg << endl;
	throw 1;
	return -1;
}

enum {
	ARG_NOFW = 256,
	ARG_NORC,
	ARG_MM,
	ARG_SHMEM,
	ARG_TESTS,
	ARG_RANDOM_TESTS,
	ARG_SEED
};

static const char *short_opts = "vCt";
static struct option long_opts[] = {
	{(char*)"verbose",  no_argument,       0, 'v'},
	{(char*)"timing",   no_argument,       0, 't'},
	{(char*)"nofw",     no_argument,       0, ARG_NOFW},
	{(char*)"norc",     no_argument,       0, ARG_NORC},
	{(char*)"mm",       no_argument,       0, ARG_MM},
	{(char*)"shmem",    no_argument,       0, ARG_SHMEM},
	{(char*)"tests",    no_argument,       0, ARG_TESTS},
	{(char*)"random",   required_argument, 0, ARG_RANDOM_TESTS},
	{(char*)"seed",     required_argument, 0, ARG_SEED},
};

static void printUsage(ostream& os) {
	os << "Usage: ac [options]* <index> <patterns>" << endl;
	os << "Options:" << endl;
	os << "  --mm                memory-mapped mode" << endl;
	os << "  --shmem             shared memory mode" << endl;
	os << "  --nofw              don't align forward-oriented read" << endl;
	os << "  --norc              don't align reverse-complemented read" << endl;
	os << "  -t/--timing         show timing information" << endl;
	os << "  -v/--verbose        talkative mode" << endl;
}

bool gNorc = false;
bool gNofw = false;
int gVerbose = 0;
int gGapBarrier = 1;
int gSnpPhred = 30;
bool gReportOverhangs = true;

extern void aligner_seed_tests();
extern void aligner_random_seed_tests(
	int num_tests,
	TIndexOffU qslo,
	TIndexOffU qshi,
	uint32_t seed);

/**
 * A way of feeding simply tests to the seed alignment infrastructure.
 */
int main(int argc, char **argv) {
	bool useMm = false;
	bool useShmem = false;
	bool mmSweep = false;
	bool noRefNames = false;
	bool sanity = false;
	bool timing = false;
	int option_index = 0;
	int seed = 777;
	int next_option;
	do {
		next_option = getopt_long(
			argc, argv, short_opts, long_opts, &option_index);
		switch (next_option) {
			case 'v':       gVerbose = true; break;
			case 't':       timing   = true; break;
			case ARG_NOFW:  gNofw    = true; break;
			case ARG_NORC:  gNorc    = true; break;
			case ARG_MM:    useMm    = true; break;
			case ARG_SHMEM: useShmem = true; break;
			case ARG_SEED:  seed = parseInt("", optarg); break;
			case ARG_TESTS: {
				aligner_seed_tests();
				aligner_random_seed_tests(
					100,     // num references
					100,   // queries per reference lo
					400,   // queries per reference hi
					18);   // pseudo-random seed
				return 0;
			}
			case ARG_RANDOM_TESTS: {
				seed = parseInt("", optarg);
				aligner_random_seed_tests(
					100,   // num references
					100,   // queries per reference lo
					400,   // queries per reference hi
					seed); // pseudo-random seed
				return 0;
			}
			case -1: break;
			default: {
				cerr << "Unknown option: " << (char)next_option << endl;
				printUsage(cerr);
				exit(1);
			}
		}
	} while(next_option != -1);
	char *reffn;
	if(optind >= argc) {
		cerr << "No reference; quitting..." << endl;
		return 1;
	}
	reffn = argv[optind++];
	if(optind >= argc) {
		cerr << "No reads; quitting..." << endl;
		return 1;
	}
	string ebwtBase(reffn);
	BitPairReference ref(
		ebwtBase,    // base path
		false,       // whether we expect it to be colorspace
		sanity,      // whether to sanity-check reference as it's loaded
		NULL,        // fasta files to sanity check reference against
		NULL,        // another way of specifying original sequences
		false,       // true -> infiles (2 args ago) contains raw seqs
		useMm,       // use memory mapping to load index?
		useShmem,    // use shared memory (not memory mapping)
		mmSweep,     // touch all the pages after memory-mapping the index
		gVerbose,    // verbose
		gVerbose);   // verbose but just for startup messages
	Timer *t = new Timer(cerr, "Time loading fw index: ", timing);
	Ebwt ebwtFw(
		ebwtBase,
		false,       // index is colorspace
		0,           // don't need entireReverse for fw index
		true,        // index is for the forward direction
		-1,          // offrate (irrelevant)
		useMm,       // whether to use memory-mapped files
		useShmem,    // whether to use shared memory
		mmSweep,     // sweep memory-mapped files
		!noRefNames, // load names?
		false,       // load SA sample?
		true,        // load ftab?
		true,        // load rstarts?
		NULL,        // reference map, or NULL if none is needed
		gVerbose,    // whether to be talkative
		gVerbose,    // talkative during initialization
		false,       // handle memory exceptions, don't pass them up
		sanity);
	delete t;
	t = new Timer(cerr, "Time loading bw index: ", timing);
	Ebwt ebwtBw(
		ebwtBase + ".rev",
		false,       // index is colorspace
		1,           // need entireReverse
		false,       // index is for the backward direction
		-1,          // offrate (irrelevant)
		useMm,       // whether to use memory-mapped files
		useShmem,    // whether to use shared memory
		mmSweep,     // sweep memory-mapped files
		!noRefNames, // load names?
		false,       // load SA sample?
		true,        // load ftab?
		false,       // load rstarts?
		NULL,        // reference map, or NULL if none is needed
		gVerbose,    // whether to be talkative
		gVerbose,    // talkative during initialization
		false,       // handle memory exceptions, don't pass them up
		sanity);
	delete t;
	for(int i = optind; i < argc; i++) {
	}
}
#endif
