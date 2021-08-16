/*
 * Generate the distance/affinity matrix that approximates the probability that two cells i and j
 * have the same genotype. The input is a pre-processed mpileup, which contains all of the sequenced
 * bases (piled up) for each position as generated by preprocessing.py.
 * Each line in the file has the form:
 * chromosome_id    position    coverage    bases   cells   read_ids
 * For example the line:
 * 22      10719571        2       TAG      0,0,3  read_id1,read_id2,read_id3
 * Means that at position 10719571 of chromosome 22, we read 'T' and 'A' in cell 0 and 'G' in cell 3
 */

#include "similarity_matrix.hpp"

#include "util/logger.hpp"
#include "util/mat.hpp"
#include "util/util.hpp"

#include <omp.h>
#include <progress_bar/progress_bar.hpp>

#include <deque>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <vector>

#include <cassert>
#include <cmath>

/**
 * If set to true, it will emulate the behavior in the Python 02_similarity_matrix.py script, which
 * in the case of overlapping paired end reads, it simply selects the first read, even if the reads
 * are different. If false, overlapping paired end reads with mismatches are ignored.
 */
constexpr bool emulate_python = false;

/** Caches a bunch of combinations and powers used again and again in the computation */
struct Cache {
    const double epsilon;
    const double h;

    // probability that two same letters will be read as different
    const double theta;
    const double theta2 = theta * theta;
    const double p_same_diff = 2 * theta * (1 - theta) + 2 * theta2 / 3;
    // probability that two same letters will be read as same
    const double p_same_same = 1 - p_same_diff;
    // probability that two different letters will be read as same
    const double p_diff_same = 2 * (1 - theta) * theta / 3 + 2 * theta2 / 9;
    // probability that two different letters will be read as different
    const double p_diff_diff = 1 - p_diff_same;

    std::vector<double> pow_p_same_same = { 1, p_same_same };
    std::vector<double> pow_p_same_diff = { 1, p_same_diff };
    std::vector<double> pow_p_diff_same = { 1, p_diff_same };
    std::vector<double> pow_p_diff_diff = { 1, p_diff_diff };

    std::vector<double> pow_1_h_epsilon = { 1, 1 - epsilon - h };
    std::vector<double> pow_1_h_epsilon2 = { 1, 1 - epsilon * 0.5 - h };
    std::vector<double> pow_h_epsilon2 = { 1, h + epsilon * 0.5 };
    std::vector<double> pow_h = { 1, h };
    std::vector<double> pow_epsilon = { 1, epsilon };
    std::vector<double> pow_0_5 = { 1, 0.5 };
    std::vector<double> pow_pss_pds = { 1, p_same_same + p_diff_same };
    std::vector<double> pow_psd_pdd = { 1, p_same_diff + p_diff_diff };

    Vec2u64 comb = { { 1 }, { 1, 1 } };


    /**
     * @param mutation_rate estimated mutation rate
     * @param homozygous_rate estimated probability that a loci is heterozygous
     * @param seq_error_rate estimated error rate in the sequencing technology
     */
    Cache(double mutation_rate,
          double homozygous_rate,
          double seq_error_rate,
          uint32_t max_read_size)
        : epsilon(mutation_rate), h(homozygous_rate), theta(seq_error_rate) {
        auto extend = [](std::vector<double> &a) { a.push_back(a.back() * a[1]); };
        for (uint32_t p = 2; p < max_read_size; ++p) {
            extend(pow_p_same_same);
            extend(pow_p_same_diff);
            extend(pow_p_diff_same);
            extend(pow_p_diff_diff);
            extend(pow_1_h_epsilon);
            extend(pow_1_h_epsilon2);
            extend(pow_h_epsilon2);
            extend(pow_h);
            extend(pow_epsilon);
            extend(pow_0_5);
            extend(pow_pss_pds);
            extend(pow_psd_pdd);
            std::vector<uint64_t> new_comb(p + 1);
            new_comb[0] = 1;
            new_comb.back() = 1;
            for (uint32_t i = 1; i < comb.back().size(); ++i) {
                new_comb[i] = comb.back()[i - 1] + comb.back()[i];
            }
            comb.push_back(std::move(new_comb));
        }
    }
};

/**
 * Retrieve or compute the probability of having (x_s, x_d) matches/mismatches assuming cells have
 * different genotypes.
 * @param x_s the number of matches
 * @param x_d the number of mismatches
 * @param c caches frequently computed values, such as combinations and powers of h and
 * epsilon
 * @param[in, out] log_probs table containing the cached log probabilities of cells being
 * *different* for x_s and x_d; if the table is empty at the desired location, the value is computed
 * @param[in, out] combs_xs_xd number of times we have seen a combination of x_s and x_d
 */
double log_prob_diff_genotype(uint32_t x_s, uint32_t x_d, const Cache &c, Matd &log_probs) {
    if (log_probs(x_s, x_d) != std::numeric_limits<double>::max()) {
        return log_probs(x_s, x_d);
    }
    double prob = 0;
    for (uint32_t k = 0; k <= x_s; ++k) {
        for (uint32_t l = 0; l <= x_d; l++) {
            for (uint32_t p = 0; p <= x_s - k; ++p) {
                for (uint32_t q = 0; q <= x_d - l; ++q) {
                    prob += c.comb[x_s][k] * c.comb[x_d][l] * c.comb[x_s - k][p]
                            * c.comb[x_d - l][q] * c.pow_1_h_epsilon[k + l] * 0.5
                            * (c.pow_p_same_same[k] * c.pow_p_same_diff[l]
                               + c.pow_p_diff_same[k] * c.pow_p_diff_diff[l])
                            * c.pow_epsilon[x_s + x_d - k - l - p - q]
                            * c.pow_0_5[x_s + x_d - k - l - p - q] * c.pow_pss_pds[x_s - k - p]
                            * c.pow_psd_pdd[x_d - l - q] * c.pow_h[p + q] * c.pow_p_same_same[p]
                            * c.pow_p_same_diff[q];
                }
            }
        }
    }
    prob *= c.comb[x_s + x_d][x_s];
    log_probs(x_s, x_d) = log(prob);
    return log_probs(x_s, x_d);
}

/**
 * Retrieve or compute the probability of having (x_s, x_d) matches/mismatches assuming cells have
 * the same genotype.
 * @param x_s the number of matches
 * @param x_d the number of mismatches
 * @param c caches frequently computed values, such as combinations and powers of h and
 * epsilon
 * @param[in, out] log_probs table containing the cached log probabilities of cells having the samge
 * genotype given x_s and x_d; if the table is empty at the desired location, the value is computed
 */
double log_prob_same_genotype(uint32_t x_s, uint32_t x_d, const Cache &c, Matd &log_probs) {
    if (log_probs(x_s, x_d) != std::numeric_limits<double>::max()) {
        return log_probs(x_s, x_d);
    }
    double p = 0;
    for (uint32_t k = 0; k <= x_s; ++k) {
        for (uint32_t l = 0; l <= x_d; l++) {
            p += c.comb[x_s][k] * c.comb[x_d][l] * c.pow_1_h_epsilon2[k + l] * 0.5
                    * (c.pow_p_same_same[k] * c.pow_p_same_diff[l]
                       + c.pow_p_diff_same[k] * c.pow_p_diff_diff[l])
                    * c.pow_h_epsilon2[x_s + x_d - k - l] * c.pow_p_same_same[x_s - k]
                    * c.pow_p_same_diff[x_d - l];
        }
    }
    p *= c.comb[x_s + x_d][x_s];
    log_probs(x_s, x_d) = log(p);
    return log_probs(x_s, x_d);
}

struct Read {
    std::vector<char> bases; // the DNA sequence corresponding to this read
    uint32_t cell_id;
    // the positions for each base in bases (they are not necessarily continuous, as only relevant
    // positions are kept)
    std::vector<uint32_t> pos;
    // the start position for the read; typically, this is equal to pos[0], with the exception being
    // when the first position was removed, because the paired reads overlapped and disagreed
    uint32_t start;
};

/**
 * Compares the read at #start_idx with all the subsequent reads (as determined by #active_keys) and
 * updates mat_same and mat_diff accordingly. Well it doesn't update mat_same and mat_diff directly
 * in order to avoid a critical section, it pushes the updates into #updates_same and #updates_diff.
 */
void compare_with_reads(const std::unordered_map<uint32_t, Read> &active_reads,
                        const std::deque<uint32_t> &active_keys,
                        uint32_t start_idx,
                        const std::vector<uint32_t> &cell_id_to_cell_idx,
                        const Cache &cache,
                        Vec2<std::tuple<uint32_t, uint32_t, double>> &updates_same,
                        Vec2<std::tuple<uint32_t, uint32_t, double>> &updates_diff,
                        Matd &log_probs_same,
                        Matd &log_probs_diff,
                        Mat32u &combs_xs_xd) {
    const Read &read1 = active_reads.at(active_keys.at(start_idx));
    if (read1.pos.empty()) {
        return;
    }
    for (uint32_t idx = start_idx + 1; idx < active_keys.size(); ++idx) {
        const Read &read2 = active_reads.at(active_keys[idx]);
        if (read2.pos.empty()) {
            continue;
        }
        uint32_t index1 = cell_id_to_cell_idx[read1.cell_id];
        uint32_t index2 = cell_id_to_cell_idx[read2.cell_id];

        // if a removed paired read that doesn't match happens to be the first read, then it's
        // not anymore guaranteed that the active_reads are sorted by the first position
        assert(!emulate_python || read1.pos.front() <= read2.pos.front());

        if (index1 == index2 || read1.pos.back() < read2.pos.front()) {
            continue; // read intersection is void, or reads are from the same cell
        }

        //  read is not from the same cell -> count the number of matches and mismatches in the
        //  overlap (all active reads include the current position in the genome, so they overlap)
        uint32_t x_s = 0;
        uint32_t x_d = 0;
        for (uint32_t idx1 = 0, idx2 = 0; idx1 < read1.bases.size() && idx2 < read2.bases.size();) {
            if (read1.pos[idx1] == read2.pos[idx2]) {
                toupper(read1.bases[idx1++]) == toupper(read2.bases[idx2++]) ? x_s++ : x_d++;
            } else {
                read1.pos[idx1] < read2.pos[idx2] ? idx1++ : idx2++;
            }
        }

        if (x_s == 0 && x_d == 0) {
            continue; // the 2 reads have no overlapping positions, nothing to do
        }

        combs_xs_xd(x_s, x_d) += 2; // +2 just to emulate the Python version

        // update the distance matrices
        updates_same[omp_get_thread_num()].push_back(std::make_tuple(
                index1, index2, log_prob_same_genotype(x_s, x_d, cache, log_probs_same)));
        updates_diff[omp_get_thread_num()].push_back(std::make_tuple(
                index1, index2, log_prob_diff_genotype(x_s, x_d, cache, log_probs_diff)));
    }
}

/** Applies the updates in the updates vector to mat */
void apply_updates(Vec2<std::tuple<uint32_t, uint32_t, double>> &updates, Matd &mat) {
    for (auto &update : updates) {
        for (auto [i, j, v] : update) {
            mat(i, j) += v;
            mat(j, i) = mat(i, j);
        }
        update.resize(0);
    }
}

Normalization to_enum(const std::string &normalization) {
    if (normalization == "ADD_MIN") {
        return Normalization::ADD_MIN;
    } else if (normalization == "EXPONENTIATE") {
        return Normalization::EXPONENTIATE;
    } else if (normalization == "SCALE_MAX_1") {
        return Normalization::SCALE_MAX_1;
    } else {
        throw std::logic_error("Invalid normalization: " + normalization);
    }
}

/**
 * Normalizes the similarity matrix according to the method specified in #normalization.
 */
void normalize(const std::string &normalization, Matd *similarity_matrix) {
    logger()->trace("Normalizing similarity matrix...");
    Matd &sim_mat = *similarity_matrix;
    switch (to_enum(normalization)) {
        case Normalization::ADD_MIN:
            sim_mat *= -1;
            sim_mat += std::abs(sim_mat.min());
            break;
        case Normalization::EXPONENTIATE:
            // compute 1/(1+exp(mat))
            sim_mat.exp();
            sim_mat += 1;
            sim_mat.inv();
            sim_mat.fill_diagonal(0);
            break;
        case Normalization::SCALE_MAX_1:
            // scale the exponentiated version so that max. element (excluding diagonal) is 1
            sim_mat.fill_diagonal(0);
            sim_mat *= (1. / sim_mat.max());
            break;
    }
    sim_mat.fill_diagonal(0);
}

/**
 * Compute mat_same (the matrix giving probabilities of cells i and j given they are in
 * the same cluster) and mat_diff (prob. of cells i and j given they are in different clusters)
 */
Matd computeSimilarityMatrix(const std::vector<std::vector<PosData>> &pos_data,
                             uint32_t num_cells,
                             uint32_t max_fragment_length,
                             const std::vector<uint32_t> &cell_id_to_cell_idx,
                             double mutation_rate,
                             double homozygous_rate,
                             double seq_error_rate,
                             const uint32_t num_threads,
                             const std::string &marker,
                             const std::string &normalization) {
    // distance matrices - the desired result of the computation
    Matd mat_same = Matd::zeros(num_cells, num_cells);
    Matd mat_diff = Matd::zeros(num_cells, num_cells);

    // stores temp updates to mat_same and mat_diff in order to avoid a critical section
    Vec2<std::tuple<uint32_t, uint32_t, double>> updates_same(num_threads);
    Vec2<std::tuple<uint32_t, uint32_t, double>> updates_diff(num_threads);

    // arrays with values of already computed probabilities, max_double if not yet computed
    Matd log_probs_same = Matd::fill(max_fragment_length, max_fragment_length,
                                     std::numeric_limits<double>::max());
    Matd log_probs_diff = Matd::fill(max_fragment_length, max_fragment_length,
                                     std::numeric_limits<double>::max());

    // count how many times we have seen a given combination of identical (x_s) and different (x_d)
    // bases in all pairs of overlapping reads
    std::vector<Mat32u> combs_xs_xd(num_threads);
    for (auto &c : combs_xs_xd) {
        c = Mat32u::zeros(max_fragment_length, max_fragment_length);
    }

    // key: read id of an active read
    // value: A Read struct, containing: sequence, line number, cell_id and position in genome
    std::unordered_map<uint32_t, Read> active_reads;

    Cache cache(mutation_rate, homozygous_rate, seq_error_rate,
                max_fragment_length); // store intermediate values to avoid recomputation

    uint64_t total_positions = 0;
    for (const auto &v : pos_data) {
        total_positions += v.size();
    }

    ProgressBar progress(total_positions, "Processed:", std::cout);
    if (progress.is_terminal()) {
        progress.SetFrequencyUpdate(std::max(uint64_t(1), total_positions / 100));
    }
    std::deque<uint32_t> active_keys;
    // the number of completed DNA fragments, i.e. reads that started max_fragment_length ago
    uint32_t completed = 0;
    for (const auto &chromosome_data : pos_data) {
        for (const PosData &pd : chromosome_data) {
            // update the number of complete reads
            for (uint32_t i = completed; i < active_keys.size()
                 && active_reads[active_keys[i]].start + max_fragment_length <= pd.position;
                 ++i) {
                ++completed;
            }

            constexpr uint32_t BATCH_SIZE = 4;
            // if we batched up enough completed reads, process them in parallel
            if (completed >= BATCH_SIZE * num_threads) {
#pragma omp parallel for schedule(static, BATCH_SIZE) num_threads(num_threads)
                for (uint32_t i = 0; i < completed; ++i) {
                    // compute its overlaps with all other active reads, i.e. all
                    // reads that have some bases in the last max_fragment_length positions
                    compare_with_reads(active_reads, active_keys, i, cell_id_to_cell_idx, cache,
                                       updates_same, updates_diff, log_probs_same, log_probs_diff,
                                       combs_xs_xd[omp_get_thread_num()]);
                }
                apply_updates(updates_same, mat_same);
                apply_updates(updates_diff, mat_diff);
                // remove processed reads
                for (uint32_t i = 0; i < completed; ++i) {
                    active_reads.erase(active_keys.front());
                    active_keys.pop_front();
                }
                completed = 0;
            }

            // update active_reads with the reads from the current position
            for (uint32_t i = 0; i < pd.read_ids.size(); ++i) {
                auto read_it = active_reads.find(pd.read_ids[i]);
                const char curr_base = pd.base(i);
                if (read_it == active_reads.end()) { // a new read just started
                    Read read = { { curr_base }, pd.cell_id(i), { pd.position }, pd.position };
                    active_reads[pd.read_ids[i]] = read;
                    active_keys.push_back(pd.read_ids[i]);
                } else {
                    Read &read = read_it->second;
                    // if we have two reads at the same position (happens due to paired-end
                    // sequencing)
                    if (!read.pos.empty() && read.pos.back() == pd.position) {
                        // different reads at the same position, so we remove the existing read, as
                        // there's a 50% chance of error
                        if (!emulate_python && toupper(read.bases.back()) != toupper(curr_base)) {
                            read.bases.pop_back();
                            read.pos.pop_back();
                        }
                        continue;
                    }

                    // <= in case emulate_python is on, otherwise we could use <
                    assert(read.pos.empty() || read.pos.back() <= pd.position);
                    // add a new base to an existing read
                    read.bases.push_back(curr_base);
                    read.pos.push_back(pd.position);
                }
            }

            progress += 1;
        }
        active_reads.clear();
        active_keys.clear();
    }

    // in the end, process all the leftover active reads
#pragma omp parallel for num_threads(num_threads)
    for (uint32_t i = 0; i < active_keys.size(); ++i) {
        // compare with all other reads in active_reads
        compare_with_reads(active_reads, active_keys, i, cell_id_to_cell_idx, cache, updates_same,
                           updates_diff, log_probs_same, log_probs_diff,
                           combs_xs_xd[omp_get_thread_num()]);
    }

    apply_updates(updates_same, mat_same);
    apply_updates(updates_diff, mat_diff);
    Mat32u combs_all = Mat32u::zeros(max_fragment_length, max_fragment_length);
    for (const auto &comb : combs_xs_xd) {
        combs_all += comb;
    }

    // compute log(P(diff)/P(same))
    mat_diff -= mat_same;

    normalize(normalization, &mat_diff);

    return mat_diff;
}
