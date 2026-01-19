#ifndef VELOX_ADVANCED_H
#define VELOX_ADVANCED_H

#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>

typedef int64_t velox_sample_t;

class VeloxOptimizer {
public:
    // --- 1. SPARSE DETECTION ---
    static bool IsSilence(const std::vector<velox_sample_t>& block, int threshold = 0) {
        for (auto s : block) {
            if (std::abs(s) > threshold) return false;
        }
        return true;
    }

    // --- 2. LONG-TERM PREDICTION (LTP / 3D Search) ---
    struct MatchResult {
        bool found;
        int lag;        
        int gain_shift; 
    };
    
    static MatchResult FindBestMatch(const std::vector<velox_sample_t>& history, 
                                     const std::vector<velox_sample_t>& target) {
        MatchResult res = {false, 0, 256}; 
        size_t n = target.size();
        size_t h_size = history.size();
        if (h_size < n * 2) return res; 
        size_t search_limit = std::min(h_size - n, (size_t)48000); 
        size_t start_idx = h_size - search_limit;
        uint64_t best_sad = UINT64_MAX; 
        uint64_t target_energy = 0;
        for(auto s : target) target_energy += std::abs(s);
        if(target_energy == 0) return res;
        for (size_t i = start_idx; i < h_size - n; i += 4) {
            uint64_t sad = 0;
            for (size_t j = 0; j < n; j += 8) { 
                sad += std::abs(target[j] - history[i + j]);
                if (sad > best_sad) break; 
            }

            if (sad < best_sad) {
                uint64_t full_sad = 0;
                for (size_t j = 0; j < n; j++) {
                    full_sad += std::abs(target[j] - history[i + j]);
                }
                
                if (full_sad < best_sad) {
                    best_sad = full_sad;
                    res.lag = (int)(h_size - i);
                    res.found = true;
                }
            }
        }

        if (res.found && best_sad < (target_energy * 0.7)) {
            return res;
        } else {
            res.found = false;
            return res;
        }
    }

    static void ApplyLTP(std::vector<velox_sample_t>& target, 
                         const std::vector<velox_sample_t>& history, 
                         int lag) {
        size_t n = target.size();
        size_t h_size = history.size();
        size_t start_idx = h_size - lag;
        
        for(size_t i=0; i<n; i++) {
            target[i] -= history[start_idx + i];
        }
    }
    
    static void RestoreLTP(std::vector<velox_sample_t>& target, 
                           const std::vector<velox_sample_t>& history, 
                           int lag) {
        size_t n = target.size();
        size_t h_size = history.size();
        size_t start_idx = h_size - lag;

        for(size_t i=0; i<n; i++) {
            target[i] += history[start_idx + i];
        }
    }
};

#endif