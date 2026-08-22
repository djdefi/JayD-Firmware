#include "DjAutoDjPlanner.h"

bool DjAutoDjPlanner::selectNext(const AutoDjCandidate* candidates, uint8_t count, AutoDjCandidate& chosen) const{
	if(candidates == nullptr || count == 0) return false;

	int bestIndex = -1;
	uint32_t bestScore = 0;
	uint16_t bestReasons = AUTO_DJ_REASON_NONE;
	bool bestIsTie = false;

	// Pass 1: candidates that clear both recency filters and the "not
	// currently in history" check.
	for(uint8_t i = 0; i < count; i++){
		const AutoDjCandidate& candidate = candidates[i];
		if(!candidate.identity.valid()) continue;
		if(history.wasRecentlyPlayed(candidate.identity, AUTO_DJ_DEFAULT_RECENT_EXCLUSION)) continue;
		if(history.artistOnCooldown(candidate.artistHash, AUTO_DJ_DEFAULT_ARTIST_EXCLUSION)) continue;
		if(history.titleOnCooldown(candidate.titleHash, AUTO_DJ_DEFAULT_TITLE_EXCLUSION)) continue;

		uint16_t reasons = AUTO_DJ_REASON_FRESH | AUTO_DJ_REASON_ARTIST_VARIETY | AUTO_DJ_REASON_TITLE_VARIETY;
		// Conservative: without metadata we never fabricate a harmonic/beat
		// match. hasMetadata candidates get their (Coach-provided) score;
		// everything else is a flat, equally-ranked fallback.
		const uint32_t score = candidate.hasMetadata ? candidate.score + 1 : 1;
		reasons |= candidate.hasMetadata ? AUTO_DJ_REASON_METADATA_MATCH : AUTO_DJ_REASON_CONSERVATIVE_FALLBACK;

		if(bestIndex < 0 || score > bestScore){
			bestIndex = i;
			bestScore = score;
			bestReasons = reasons;
			bestIsTie = false;
		} else if(score == bestScore && autoDjIdentityLess(candidate.identity, candidates[bestIndex].identity)){
			bestIndex = i;
			bestReasons = reasons;
			bestIsTie = true;
		}
	}

	if(bestIndex < 0){
		// Every eligible candidate was excluded by recency. The queue must
		// never starve permanently, so relax the exclusion and fall back to
		// a stable identity ordering - still never a fabricated match.
		for(uint8_t i = 0; i < count; i++){
			const AutoDjCandidate& candidate = candidates[i];
			if(!candidate.identity.valid()) continue;
			if(bestIndex < 0 || autoDjIdentityLess(candidate.identity, candidates[bestIndex].identity)){
				bestIndex = i;
				bestReasons = AUTO_DJ_REASON_CONSERVATIVE_FALLBACK | AUTO_DJ_REASON_TIE_BREAK;
			}
		}
	} else if(bestIsTie){
		bestReasons |= AUTO_DJ_REASON_TIE_BREAK;
	}

	if(bestIndex < 0) return false;
	chosen = candidates[bestIndex];
	chosen.reasons = bestReasons;
	return true;
}
