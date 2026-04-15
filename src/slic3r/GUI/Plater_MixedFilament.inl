namespace {
wxColour parse_mixed_color(const std::string &value)
{
    wxColour color(value);
    if (!color.IsOk())
        color = wxColour("#26A69A");
    return color;
}

wxColour blend_pair_filament_mixer(const wxColour &left, const wxColour &right, float t)
{
    const wxColour safe_left = left.IsOk() ? left : wxColour("#26A69A");
    const wxColour safe_right = right.IsOk() ? right : wxColour("#26A69A");

    unsigned char out_r = static_cast<unsigned char>(safe_left.Red());
    unsigned char out_g = static_cast<unsigned char>(safe_left.Green());
    unsigned char out_b = static_cast<unsigned char>(safe_left.Blue());
    ::Slic3r::filament_mixer_lerp(static_cast<unsigned char>(safe_left.Red()),
                                  static_cast<unsigned char>(safe_left.Green()),
                                  static_cast<unsigned char>(safe_left.Blue()),
                                  static_cast<unsigned char>(safe_right.Red()),
                                  static_cast<unsigned char>(safe_right.Green()),
                                  static_cast<unsigned char>(safe_right.Blue()),
                                  std::clamp(t, 0.f, 1.f),
                                  &out_r, &out_g, &out_b);
    return wxColour(out_r, out_g, out_b);
}

wxColour blend_multi_filament_mixer(const std::vector<wxColour> &colors, const std::vector<double> &weights)
{
    if (colors.empty() || weights.empty())
        return wxColour("#26A69A");

    unsigned char out_r = 0;
    unsigned char out_g = 0;
    unsigned char out_b = 0;
    double accumulated_weight = 0.0;
    bool has_color = false;

    for (size_t i = 0; i < colors.size() && i < weights.size(); ++i) {
        const double weight = std::max(0.0, weights[i]);
        if (weight <= 0.0)
            continue;

        const wxColour safe = colors[i].IsOk() ? colors[i] : wxColour("#26A69A");
        const unsigned char r = static_cast<unsigned char>(safe.Red());
        const unsigned char g = static_cast<unsigned char>(safe.Green());
        const unsigned char b = static_cast<unsigned char>(safe.Blue());

        if (!has_color) {
            out_r = r;
            out_g = g;
            out_b = b;
            accumulated_weight = weight;
            has_color = true;
            continue;
        }

        const double new_total = accumulated_weight + weight;
        if (new_total <= 0.0)
            continue;
        const float t = float(weight / new_total);
        ::Slic3r::filament_mixer_lerp(out_r, out_g, out_b, r, g, b, t, &out_r, &out_g, &out_b);
        accumulated_weight = new_total;
    }

    if (!has_color)
        return wxColour("#26A69A");

    return wxColour(out_r, out_g, out_b);
}

wxString normalize_color_match_hex(const wxString &value)
{
    wxString normalized = value;
    normalized.Trim(true);
    normalized.Trim(false);
    normalized.MakeUpper();
    if (!normalized.empty() && normalized[0] != '#')
        normalized.Prepend("#");
    return normalized;
}

bool try_parse_color_match_hex(const wxString &value, wxColour &color_out)
{
    const wxString normalized = normalize_color_match_hex(value);
    if (normalized.length() != 7)
        return false;

    for (size_t idx = 1; idx < normalized.length(); ++idx) {
        const unsigned char ch = static_cast<unsigned char>(normalized[idx]);
        if (!std::isxdigit(ch))
            return false;
    }

    wxColour parsed(normalized);
    if (!parsed.IsOk())
        return false;

    color_out = parsed;
    return true;
}

std::vector<unsigned int> decode_color_match_gradient_ids(const std::string &value)
{
    std::vector<unsigned int> ids;
    bool seen[10] = { false };
    for (const char ch : value) {
        if (ch < '1' || ch > '9')
            continue;
        const unsigned int id = unsigned(ch - '0');
        if (seen[id])
            continue;
        seen[id] = true;
        ids.emplace_back(id);
    }
    return ids;
}

std::vector<int> decode_color_match_gradient_weights(const std::string &value, size_t expected_components)
{
    std::vector<int> weights;
    if (value.empty() || expected_components == 0)
        return weights;

    std::string token;
    for (const char ch : value) {
        if (ch >= '0' && ch <= '9') {
            token.push_back(ch);
            continue;
        }
        if (!token.empty()) {
            weights.emplace_back(std::max(0, std::atoi(token.c_str())));
            token.clear();
        }
    }
    if (!token.empty())
        weights.emplace_back(std::max(0, std::atoi(token.c_str())));
    if (weights.size() != expected_components)
        weights.clear();
    return weights;
}

std::vector<int> normalize_color_match_weights(const std::vector<int> &weights, size_t count)
{
    std::vector<int> out = weights;
    if (out.size() != count)
        out.assign(count, count > 0 ? int(100 / count) : 0);

    int sum = 0;
    for (int &value : out) {
        value = std::max(0, value);
        sum += value;
    }
    if (sum <= 0 && count > 0) {
        out.assign(count, 0);
        out[0] = 100;
        return out;
    }

    std::vector<double> remainders(count, 0.0);
    int assigned = 0;
    for (size_t idx = 0; idx < count; ++idx) {
        const double exact = 100.0 * double(out[idx]) / double(sum);
        out[idx] = int(std::floor(exact));
        remainders[idx] = exact - double(out[idx]);
        assigned += out[idx];
    }

    int missing = std::max(0, 100 - assigned);
    while (missing > 0) {
        size_t best_idx = 0;
        double best_remainder = -1.0;
        for (size_t idx = 0; idx < remainders.size(); ++idx) {
            if (remainders[idx] > best_remainder) {
                best_remainder = remainders[idx];
                best_idx = idx;
            }
        }
        ++out[best_idx];
        remainders[best_idx] = 0.0;
        --missing;
    }

    return out;
}

std::vector<unsigned int> build_color_match_sequence(const std::vector<unsigned int> &ids, const std::vector<int> &weights);
wxColour blend_sequence_filament_mixer(const std::vector<wxColour> &palette, const std::vector<unsigned int> &sequence);

bool color_match_weights_within_range(const std::vector<int> &weights, int min_component_percent)
{
    if (min_component_percent <= 0)
        return true;

    const int min_allowed = std::clamp(min_component_percent, 0, 50);
    int active_components = 0;
    for (const int weight : weights) {
        if (weight <= 0)
            continue;
        ++active_components;
        if (weight < min_allowed)
            return false;
    }
    return active_components >= 2;
}

bool color_match_raw_weights_within_range(const std::vector<double> &weights, int min_component_percent)
{
    if (min_component_percent <= 0)
        return true;

    const double min_allowed = double(std::clamp(min_component_percent, 0, 50));
    int active_components = 0;
    for (const double weight : weights) {
        if (weight <= 1e-4)
            continue;
        ++active_components;
        if (weight * 100.0 + 1e-6 < min_allowed)
            return false;
    }
    return active_components >= 2;
}

} // end anonymous namespace (temporarily) for MixedColorMatchRecipeResult

#ifndef MIXED_COLOR_MATCH_RECIPE_RESULT_DEFINED
struct MixedColorMatchRecipeResult
{
    bool         cancelled     = false;
    bool         valid         = false;
    unsigned int component_a   = 1;
    unsigned int component_b   = 2;
    int          mix_b_percent = 50;
    std::string  manual_pattern;
    std::string  gradient_component_ids;
    std::string  gradient_component_weights;
    wxColour     preview_color = wxColour("#26A69A");
    double       delta_e       = std::numeric_limits<double>::infinity();
};
#endif

namespace { // reopen anonymous namespace

MixedColorMatchRecipeResult build_pair_color_match_candidate(const std::vector<wxColour> &palette,
                                                             unsigned int                  component_a,
                                                             unsigned int                  component_b,
                                                             int                           mix_b_percent,
                                                             int                           min_component_percent = 0)
{
    MixedColorMatchRecipeResult candidate;
    if (component_a == 0 || component_b == 0 || component_a == component_b)
        return candidate;
    if (component_a > palette.size() || component_b > palette.size())
        return candidate;
    if (!color_match_weights_within_range({ 100 - std::clamp(mix_b_percent, 0, 100), std::clamp(mix_b_percent, 0, 100) }, min_component_percent))
        return candidate;

    candidate.valid         = true;
    candidate.component_a   = component_a;
    candidate.component_b   = component_b;
    candidate.mix_b_percent = std::clamp(mix_b_percent, 0, 100);
    candidate.preview_color = blend_pair_filament_mixer(palette[component_a - 1], palette[component_b - 1],
                                                        float(candidate.mix_b_percent) / 100.f);
    return candidate;
}

MixedColorMatchRecipeResult build_multi_color_match_candidate(const std::vector<wxColour>      &palette,
                                                              const std::vector<unsigned int> &ids,
                                                              const std::vector<int>          &weights,
                                                              int                              min_component_percent = 0)
{
    MixedColorMatchRecipeResult candidate;
    if (ids.size() < 3 || ids.size() != weights.size())
        return candidate;
    if (!color_match_weights_within_range(weights, min_component_percent))
        return candidate;

    std::vector<std::pair<int, unsigned int>> weighted_ids;
    weighted_ids.reserve(ids.size());
    for (size_t idx = 0; idx < ids.size(); ++idx) {
        if (ids[idx] == 0 || ids[idx] > palette.size() || ids[idx] > 9)
            return candidate;
        if (weights[idx] <= 0)
            continue;
        weighted_ids.emplace_back(weights[idx], ids[idx]);
    }
    if (weighted_ids.size() < 3)
        return candidate;

    std::sort(weighted_ids.begin(), weighted_ids.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.first != rhs.first)
            return lhs.first > rhs.first;
        return lhs.second < rhs.second;
    });

    std::vector<unsigned int> ordered_ids;
    std::vector<int>          ordered_weights;
    ordered_ids.reserve(weighted_ids.size());
    ordered_weights.reserve(weighted_ids.size());
    for (const auto &[weight, filament_id] : weighted_ids) {
        ordered_ids.emplace_back(filament_id);
        ordered_weights.emplace_back(weight);
    }

    const std::vector<unsigned int> sequence = build_color_match_sequence(ordered_ids, ordered_weights);
    if (sequence.empty())
        return candidate;

    candidate.valid       = true;
    candidate.component_a = ordered_ids[0];
    candidate.component_b = ordered_ids[1];
    const int pair_weight_total = ordered_weights[0] + ordered_weights[1];
    candidate.mix_b_percent = pair_weight_total > 0 ?
        std::clamp(int(std::lround(100.0 * double(ordered_weights[1]) / double(pair_weight_total))), 0, 100) :
        50;
    for (const unsigned int filament_id : ordered_ids)
        candidate.gradient_component_ids.push_back(char('0' + filament_id));
    {
        std::ostringstream weights_ss;
        for (size_t weight_idx = 0; weight_idx < ordered_weights.size(); ++weight_idx) {
            if (weight_idx > 0)
                weights_ss << '/';
            weights_ss << ordered_weights[weight_idx];
        }
        candidate.gradient_component_weights = weights_ss.str();
    }
    candidate.preview_color = blend_sequence_filament_mixer(palette, sequence);
    return candidate;
}

std::vector<int> expand_color_match_recipe_weights(const MixedColorMatchRecipeResult &recipe, size_t num_physical)
{
    std::vector<int> weights(num_physical, 0);
    if (!recipe.valid || num_physical == 0)
        return weights;

    if (!recipe.gradient_component_ids.empty()) {
        const std::vector<unsigned int> ids = decode_color_match_gradient_ids(recipe.gradient_component_ids);
        const std::vector<int> raw_weights =
            normalize_color_match_weights(decode_color_match_gradient_weights(recipe.gradient_component_weights, ids.size()), ids.size());
        if (ids.size() != raw_weights.size())
            return weights;
        for (size_t idx = 0; idx < ids.size(); ++idx) {
            if (ids[idx] >= 1 && ids[idx] <= num_physical)
                weights[ids[idx] - 1] = raw_weights[idx];
        }
        return weights;
    }

    if (recipe.component_a >= 1 && recipe.component_a <= num_physical)
        weights[recipe.component_a - 1] = std::max(0, 100 - std::clamp(recipe.mix_b_percent, 0, 100));
    if (recipe.component_b >= 1 && recipe.component_b <= num_physical)
        weights[recipe.component_b - 1] = std::max(0, std::clamp(recipe.mix_b_percent, 0, 100));
    return weights;
}

std::string summarize_color_match_recipe(const MixedColorMatchRecipeResult &recipe)
{
    if (!recipe.valid)
        return {};

    std::vector<unsigned int> ids;
    std::vector<int>          weights;
    if (!recipe.gradient_component_ids.empty()) {
        ids = decode_color_match_gradient_ids(recipe.gradient_component_ids);
        weights = normalize_color_match_weights(
            decode_color_match_gradient_weights(recipe.gradient_component_weights, ids.size()), ids.size());
    } else {
        ids = { recipe.component_a, recipe.component_b };
        weights = { std::max(0, 100 - std::clamp(recipe.mix_b_percent, 0, 100)),
                    std::max(0, std::clamp(recipe.mix_b_percent, 0, 100)) };
    }
    if (ids.empty() || ids.size() != weights.size())
        return {};

    std::ostringstream out;
    for (size_t idx = 0; idx < ids.size(); ++idx) {
        if (idx > 0)
            out << '/';
        out << 'F' << ids[idx];
    }
    out << ' ';
    for (size_t idx = 0; idx < weights.size(); ++idx) {
        if (idx > 0)
            out << '/';
        out << weights[idx] << '%';
    }
    return out.str();
}

wxBitmap make_color_match_swatch_bitmap(const wxColour &color, const wxSize &size)
{
    wxBitmap bmp(size.GetWidth(), size.GetHeight());
    wxMemoryDC dc(bmp);
    dc.SetBackground(wxBrush(wxColour(255, 255, 255)));
    dc.Clear();
    dc.SetPen(wxPen(wxColour(120, 120, 120), 1));
    dc.SetBrush(wxBrush(color.IsOk() ? color : wxColour("#26A69A")));
    dc.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
    dc.SelectObject(wxNullBitmap);
    return bmp;
}

std::vector<MixedColorMatchRecipeResult> build_color_match_presets(const std::vector<std::string> &physical_colors,
                                                                   int                             min_component_percent = 0)
{
    std::vector<MixedColorMatchRecipeResult> presets;
    if (physical_colors.size() < 2)
        return presets;

    std::vector<wxColour> palette;
    palette.reserve(physical_colors.size());
    for (const std::string &hex : physical_colors)
        palette.emplace_back(parse_mixed_color(hex));

    constexpr size_t k_max_presets = 48;
    std::unordered_set<std::string> seen_colors;
    auto add_candidate = [&presets, &seen_colors](MixedColorMatchRecipeResult candidate) {
        if (!candidate.valid)
            return;
        const std::string color_key = normalize_color_match_hex(candidate.preview_color.GetAsString(wxC2S_HTML_SYNTAX)).ToStdString();
        if (color_key.empty() || !seen_colors.insert(color_key).second)
            return;
        presets.emplace_back(std::move(candidate));
    };

    constexpr int pair_ratios[] = { 25, 50, 75 };
    for (size_t left_idx = 0; left_idx < palette.size() && presets.size() < k_max_presets; ++left_idx) {
        for (size_t right_idx = left_idx + 1; right_idx < palette.size() && presets.size() < k_max_presets; ++right_idx) {
            for (const int mix_b_percent : pair_ratios) {
                add_candidate(build_pair_color_match_candidate(palette, unsigned(left_idx + 1), unsigned(right_idx + 1),
                                                               mix_b_percent, min_component_percent));
                if (presets.size() >= k_max_presets)
                    break;
            }
        }
    }

    const size_t triple_limit = std::min<size_t>(palette.size(), 6);
    const std::vector<int> equal_triple_weights = normalize_color_match_weights({ 1, 1, 1 }, 3);
    for (size_t first_idx = 0; first_idx + 2 < triple_limit && presets.size() < k_max_presets; ++first_idx) {
        for (size_t second_idx = first_idx + 1; second_idx + 1 < triple_limit && presets.size() < k_max_presets; ++second_idx) {
            for (size_t third_idx = second_idx + 1; third_idx < triple_limit && presets.size() < k_max_presets; ++third_idx) {
                const std::vector<unsigned int> ids = {
                    unsigned(first_idx + 1),
                    unsigned(second_idx + 1),
                    unsigned(third_idx + 1)
                };
                add_candidate(build_multi_color_match_candidate(palette, ids, equal_triple_weights, min_component_percent));
                for (size_t dominant_idx = 0; dominant_idx < ids.size() && presets.size() < k_max_presets; ++dominant_idx) {
                    std::vector<int> dominant_weights(ids.size(), 25);
                    dominant_weights[dominant_idx] = 50;
                    add_candidate(build_multi_color_match_candidate(palette, ids, dominant_weights, min_component_percent));
                }
            }
        }
    }

    const size_t quad_limit = std::min<size_t>(palette.size(), 5);
    for (size_t first_idx = 0; first_idx + 3 < quad_limit && presets.size() < k_max_presets; ++first_idx) {
        for (size_t second_idx = first_idx + 1; second_idx + 2 < quad_limit && presets.size() < k_max_presets; ++second_idx) {
            for (size_t third_idx = second_idx + 1; third_idx + 1 < quad_limit && presets.size() < k_max_presets; ++third_idx) {
                for (size_t fourth_idx = third_idx + 1; fourth_idx < quad_limit && presets.size() < k_max_presets; ++fourth_idx) {
                    add_candidate(build_multi_color_match_candidate(
                        palette,
                        { unsigned(first_idx + 1), unsigned(second_idx + 1), unsigned(third_idx + 1), unsigned(fourth_idx + 1) },
                        { 25, 25, 25, 25 },
                        min_component_percent));
                }
            }
        }
    }

    return presets;
}

double color_delta_e00(const wxColour &lhs, const wxColour &rhs)
{
    float lhs_l = 0.f, lhs_a = 0.f, lhs_b = 0.f;
    float rhs_l = 0.f, rhs_a = 0.f, rhs_b = 0.f;
    RGB2Lab(float(lhs.Red()), float(lhs.Green()), float(lhs.Blue()), &lhs_l, &lhs_a, &lhs_b);
    RGB2Lab(float(rhs.Red()), float(rhs.Green()), float(rhs.Blue()), &rhs_l, &rhs_a, &rhs_b);
    return double(DeltaE00(lhs_l, lhs_a, lhs_b, rhs_l, rhs_a, rhs_b));
}

std::vector<unsigned int> build_color_match_sequence(const std::vector<unsigned int> &ids, const std::vector<int> &weights)
{
    if (ids.empty() || ids.size() != weights.size())
        return {};

    constexpr int k_max_cycle = 48;

    std::vector<unsigned int> filtered_ids;
    std::vector<int>          counts;
    filtered_ids.reserve(ids.size());
    counts.reserve(weights.size());
    for (size_t idx = 0; idx < ids.size(); ++idx) {
        const int weight = std::max(0, weights[idx]);
        if (weight <= 0)
            continue;
        filtered_ids.emplace_back(ids[idx]);
        counts.emplace_back(std::max(1, int(std::round((double(weight) / 100.0) * k_max_cycle))));
    }

    if (filtered_ids.empty())
        return {};

    int cycle = std::accumulate(counts.begin(), counts.end(), 0);
    while (cycle > k_max_cycle) {
        auto it = std::max_element(counts.begin(), counts.end());
        if (it == counts.end() || *it <= 1)
            break;
        --(*it);
        --cycle;
    }

    if (cycle <= 0)
        return {};

    std::vector<unsigned int> sequence;
    sequence.reserve(size_t(cycle));
    std::vector<int> emitted(counts.size(), 0);
    for (int pos = 0; pos < cycle; ++pos) {
        size_t best_idx = 0;
        double best_score = -1e9;
        for (size_t idx = 0; idx < counts.size(); ++idx) {
            const double target = double((pos + 1) * counts[idx]) / double(std::max(1, cycle));
            const double score  = target - double(emitted[idx]);
            if (score > best_score) {
                best_score = score;
                best_idx   = idx;
            }
        }
        ++emitted[best_idx];
        sequence.emplace_back(filtered_ids[best_idx]);
    }

    return sequence;
}

wxColour blend_sequence_filament_mixer(const std::vector<wxColour> &palette, const std::vector<unsigned int> &sequence)
{
    if (palette.empty() || sequence.empty())
        return wxColour("#26A69A");

    std::vector<int> counts(palette.size() + 1, 0);
    for (const unsigned int filament_id : sequence) {
        if (filament_id == 0 || filament_id > palette.size())
            continue;
        ++counts[filament_id];
    }

    std::vector<wxColour> colors;
    std::vector<double>   weights;
    colors.reserve(palette.size());
    weights.reserve(palette.size());
    for (size_t filament_id = 1; filament_id <= palette.size(); ++filament_id) {
        if (counts[filament_id] <= 0)
            continue;
        colors.emplace_back(palette[filament_id - 1]);
        weights.emplace_back(double(counts[filament_id]));
    }

    return blend_multi_filament_mixer(colors, weights);
}

MixedColorMatchRecipeResult build_best_color_match_recipe(const std::vector<std::string> &physical_colors,
                                                          const wxColour                 &target_color,
                                                          int                             min_component_percent = 0)
{
    MixedColorMatchRecipeResult best;
    if (!target_color.IsOk() || physical_colors.size() < 2)
        return best;

    std::vector<wxColour> palette;
    palette.reserve(physical_colors.size());
    for (const std::string &hex : physical_colors)
        palette.emplace_back(parse_mixed_color(hex));

    auto consider_candidate = [&best, &target_color](MixedColorMatchRecipeResult candidate) {
        if (!candidate.valid)
            return;
        candidate.delta_e = color_delta_e00(target_color, candidate.preview_color);
        if (!best.valid || candidate.delta_e + 1e-6 < best.delta_e)
            best = std::move(candidate);
    };

    const int loop_min_weight = std::max(1, std::clamp(min_component_percent, 0, 50));
    const int loop_max_pair_weight = 100 - loop_min_weight;

    for (size_t left_idx = 0; left_idx < palette.size(); ++left_idx) {
        for (size_t right_idx = left_idx + 1; right_idx < palette.size(); ++right_idx) {
            for (int mix_b_percent = loop_min_weight; mix_b_percent <= loop_max_pair_weight; ++mix_b_percent)
                consider_candidate(build_pair_color_match_candidate(palette, unsigned(left_idx + 1), unsigned(right_idx + 1),
                                                                    mix_b_percent, min_component_percent));
        }
    }

    std::vector<std::pair<double, unsigned int>> ranked_ids;
    ranked_ids.reserve(palette.size());
    for (size_t idx = 0; idx < palette.size(); ++idx)
        ranked_ids.emplace_back(color_delta_e00(target_color, palette[idx]), unsigned(idx + 1));
    std::sort(ranked_ids.begin(), ranked_ids.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.first != rhs.first)
            return lhs.first < rhs.first;
        return lhs.second < rhs.second;
    });

    std::vector<unsigned int> candidate_pool;
    candidate_pool.reserve(std::min<size_t>(palette.size(), 12));
    auto push_unique_id = [&candidate_pool](unsigned int filament_id) {
        if (filament_id == 0 || filament_id > 9)
            return;
        if (std::find(candidate_pool.begin(), candidate_pool.end(), filament_id) == candidate_pool.end())
            candidate_pool.emplace_back(filament_id);
    };

    const size_t general_pool_limit = std::min<size_t>(ranked_ids.size(), 8);
    for (size_t idx = 0; idx < general_pool_limit; ++idx)
        push_unique_id(ranked_ids[idx].second);

    size_t direct_token_count = 0;
    for (const auto &[distance, filament_id] : ranked_ids) {
        (void) distance;
        if (filament_id < 3 || filament_id > 9)
            continue;
        push_unique_id(filament_id);
        if (++direct_token_count >= 4)
            break;
    }

    if (candidate_pool.size() < 3)
        return best;

    std::vector<unsigned int> triple_pool = candidate_pool;
    std::sort(triple_pool.begin(), triple_pool.end());
    for (size_t first_idx = 0; first_idx + 2 < triple_pool.size(); ++first_idx) {
        for (size_t second_idx = first_idx + 1; second_idx + 1 < triple_pool.size(); ++second_idx) {
            for (size_t third_idx = second_idx + 1; third_idx < triple_pool.size(); ++third_idx) {
                const std::vector<unsigned int> ids = {
                    triple_pool[first_idx],
                    triple_pool[second_idx],
                    triple_pool[third_idx]
                };
                if (std::any_of(ids.begin(), ids.end(), [](unsigned int filament_id) { return filament_id == 0 || filament_id > 9; }))
                    continue;

                for (int weight_a = loop_min_weight; weight_a <= 100 - 2 * loop_min_weight; ++weight_a) {
                    for (int weight_b = loop_min_weight; weight_a + weight_b <= 100 - loop_min_weight; ++weight_b) {
                        const int weight_c = 100 - weight_a - weight_b;
                        consider_candidate(build_multi_color_match_candidate(palette, ids, { weight_a, weight_b, weight_c },
                                                                            min_component_percent));
                    }
                }
            }
        }
    }

    if (candidate_pool.size() < 4)
        return best;

    std::vector<unsigned int> quad_pool(candidate_pool.begin(),
                                        candidate_pool.begin() + std::min<size_t>(candidate_pool.size(), 6));
    std::sort(quad_pool.begin(), quad_pool.end());
    for (size_t first_idx = 0; first_idx + 3 < quad_pool.size(); ++first_idx) {
        for (size_t second_idx = first_idx + 1; second_idx + 2 < quad_pool.size(); ++second_idx) {
            for (size_t third_idx = second_idx + 1; third_idx + 1 < quad_pool.size(); ++third_idx) {
                for (size_t fourth_idx = third_idx + 1; fourth_idx < quad_pool.size(); ++fourth_idx) {
                    const std::vector<unsigned int> ids = {
                        quad_pool[first_idx],
                        quad_pool[second_idx],
                        quad_pool[third_idx],
                        quad_pool[fourth_idx]
                    };

                    for (int weight_a = loop_min_weight; weight_a <= 100 - 3 * loop_min_weight; ++weight_a) {
                        for (int weight_b = loop_min_weight; weight_a + weight_b <= 100 - 2 * loop_min_weight; ++weight_b) {
                            for (int weight_c = loop_min_weight; weight_a + weight_b + weight_c <= 100 - loop_min_weight; ++weight_c) {
                                const int weight_d = 100 - weight_a - weight_b - weight_c;
                                consider_candidate(build_multi_color_match_candidate(
                                    palette, ids, { weight_a, weight_b, weight_c, weight_d }, min_component_percent));
                            }
                        }
                    }
                }
            }
        }
    }

    return best;
}
class MixedFilamentColorMapPanel : public wxPanel
{
public:
    MixedFilamentColorMapPanel(wxWindow                        *parent,
                               const std::vector<unsigned int> &filament_ids,
                               const std::vector<wxColour>     &palette,
                               const std::vector<int>          &initial_weights,
                               const wxSize                    &min_size)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, min_size, wxBORDER_SIMPLE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(min_size);
        m_render_timer.SetOwner(this);

        m_colors.reserve(filament_ids.size());
        for (const unsigned int filament_id : filament_ids) {
            if (filament_id >= 1 && filament_id <= palette.size())
                m_colors.emplace_back(palette[filament_id - 1]);
            else
                m_colors.emplace_back(wxColour("#26A69A"));
        }
        if (m_colors.empty())
            m_colors.emplace_back(wxColour("#26A69A"));

        set_normalized_weights(initial_weights, false);

        Bind(wxEVT_PAINT, &MixedFilamentColorMapPanel::on_paint, this);
        Bind(wxEVT_LEFT_DOWN, &MixedFilamentColorMapPanel::on_left_down, this);
        Bind(wxEVT_LEFT_UP, &MixedFilamentColorMapPanel::on_left_up, this);
        Bind(wxEVT_MOTION, &MixedFilamentColorMapPanel::on_mouse_move, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, &MixedFilamentColorMapPanel::on_capture_lost, this);
        Bind(wxEVT_SIZE, &MixedFilamentColorMapPanel::on_size, this);
        Bind(wxEVT_TIMER, &MixedFilamentColorMapPanel::on_render_timer, this, m_render_timer.GetId());
    }

    ~MixedFilamentColorMapPanel() override
    {
        if (HasCapture())
            ReleaseMouse();
        if (m_render_timer.IsRunning())
            m_render_timer.Stop();
    }

    std::vector<int> normalized_weights() const
    {
        return m_weights;
    }

    wxColour selected_color() const
    {
        std::vector<double> weights;
        weights.reserve(m_weights.size());
        for (const int weight : m_weights)
            weights.emplace_back(double(std::max(0, weight)));
        return blend_multi_filament_mixer(m_colors, weights);
    }

    void set_normalized_weights(const std::vector<int> &weights, bool notify)
    {
        m_weights = normalize_color_match_weights(weights, m_colors.size());
        initialize_cursor_from_weights();
        Refresh();
        if (notify)
            emit_changed();
    }

    void set_min_component_percent(int min_component_percent)
    {
        const int clamped = std::clamp(min_component_percent, 0, 50);
        if (m_min_component_percent == clamped)
            return;
        m_min_component_percent = clamped;
        invalidate_cached_bitmap();
        Refresh();
    }

private:
    enum class GeometryMode {
        Point,
        Line,
        Triangle,
        TriangleWithCenter,
        Radial
    };

    struct AnchorPoint {
        double x { 0.5 };
        double y { 0.5 };
    };

    struct Vec2 {
        double x { 0.0 };
        double y { 0.0 };
    };

    GeometryMode geometry_mode() const
    {
        if (m_colors.size() <= 1)
            return GeometryMode::Point;
        if (m_colors.size() == 2)
            return GeometryMode::Line;
        if (m_colors.size() == 3)
            return GeometryMode::Triangle;
        if (m_colors.size() == 4)
            return GeometryMode::TriangleWithCenter;
        return GeometryMode::Radial;
    }

    wxRect canvas_rect() const
    {
        const wxSize size = GetClientSize();
        return wxRect(0, 0, std::max(1, size.GetWidth()), std::max(1, size.GetHeight()));
    }

    static Vec2 make_vec(double x, double y)
    {
        return Vec2 { x, y };
    }

    static Vec2 add_vec(const Vec2 &lhs, const Vec2 &rhs)
    {
        return Vec2 { lhs.x + rhs.x, lhs.y + rhs.y };
    }

    static Vec2 sub_vec(const Vec2 &lhs, const Vec2 &rhs)
    {
        return Vec2 { lhs.x - rhs.x, lhs.y - rhs.y };
    }

    static Vec2 scale_vec(const Vec2 &value, double factor)
    {
        return Vec2 { value.x * factor, value.y * factor };
    }

    static double dot_vec(const Vec2 &lhs, const Vec2 &rhs)
    {
        return lhs.x * rhs.x + lhs.y * rhs.y;
    }

    static double length_sq(const Vec2 &value)
    {
        return dot_vec(value, value);
    }

    static double dist_sq(const Vec2 &lhs, const Vec2 &rhs)
    {
        return length_sq(sub_vec(lhs, rhs));
    }

    std::array<Vec2, 3> simplex_vertices() const
    {
        return { make_vec(0.50, 0.05), make_vec(0.08, 0.94), make_vec(0.92, 0.94) };
    }

    Vec2 simplex_center() const
    {
        const auto vertices = simplex_vertices();
        return make_vec((vertices[0].x + vertices[1].x + vertices[2].x) / 3.0,
                        (vertices[0].y + vertices[1].y + vertices[2].y) / 3.0);
    }

    std::vector<AnchorPoint> radial_anchor_points() const
    {
        std::vector<AnchorPoint> anchors;
        const size_t count = m_colors.size();
        anchors.reserve(count);
        if (count == 0)
            return anchors;
        if (count == 1) {
            anchors.emplace_back(AnchorPoint { 0.5, 0.5 });
            return anchors;
        }
        if (count == 2) {
            anchors.emplace_back(AnchorPoint { 0.0, 0.5 });
            anchors.emplace_back(AnchorPoint { 1.0, 0.5 });
            return anchors;
        }
        if (count == 3) {
            anchors.emplace_back(AnchorPoint { 0.0, 0.5 });
            anchors.emplace_back(AnchorPoint { 1.0, 0.0 });
            anchors.emplace_back(AnchorPoint { 1.0, 1.0 });
            return anchors;
        }
        if (count == 4) {
            anchors.emplace_back(AnchorPoint { 0.0, 0.0 });
            anchors.emplace_back(AnchorPoint { 1.0, 0.0 });
            anchors.emplace_back(AnchorPoint { 1.0, 1.0 });
            anchors.emplace_back(AnchorPoint { 0.0, 1.0 });
            return anchors;
        }

        constexpr double k_pi = 3.14159265358979323846;
        const double center_x = 0.5;
        const double center_y = 0.5;
        const double radius = 0.45;
        for (size_t idx = 0; idx < count; ++idx) {
            const double angle = (2.0 * k_pi * double(idx)) / double(count);
            anchors.emplace_back(AnchorPoint { center_x + radius * std::cos(angle), center_y + radius * std::sin(angle) });
        }
        return anchors;
    }

    std::vector<AnchorPoint> anchor_points() const
    {
        std::vector<AnchorPoint> anchors;
        switch (geometry_mode()) {
        case GeometryMode::Point:
            anchors.emplace_back(AnchorPoint { 0.5, 0.5 });
            break;
        case GeometryMode::Line:
            anchors.emplace_back(AnchorPoint { 0.06, 0.5 });
            anchors.emplace_back(AnchorPoint { 0.94, 0.5 });
            break;
        case GeometryMode::Triangle: {
            const auto vertices = simplex_vertices();
            for (const Vec2 &vertex : vertices)
                anchors.emplace_back(AnchorPoint { vertex.x, vertex.y });
            break;
        }
        case GeometryMode::TriangleWithCenter: {
            const auto vertices = simplex_vertices();
            for (const Vec2 &vertex : vertices)
                anchors.emplace_back(AnchorPoint { vertex.x, vertex.y });
            const Vec2 center = simplex_center();
            anchors.emplace_back(AnchorPoint { center.x, center.y });
            break;
        }
        case GeometryMode::Radial:
            anchors = radial_anchor_points();
            break;
        }
        return anchors;
    }

    static std::array<double, 3> triangle_barycentric(const Vec2 &point, const std::array<Vec2, 3> &triangle)
    {
        const Vec2 &a = triangle[0];
        const Vec2 &b = triangle[1];
        const Vec2 &c = triangle[2];
        const double denom = ((b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y));
        if (std::abs(denom) <= 1e-9)
            return { 1.0, 0.0, 0.0 };
        const double w0 = ((b.y - c.y) * (point.x - c.x) + (c.x - b.x) * (point.y - c.y)) / denom;
        const double w1 = ((c.y - a.y) * (point.x - c.x) + (a.x - c.x) * (point.y - c.y)) / denom;
        const double w2 = 1.0 - w0 - w1;
        return { w0, w1, w2 };
    }

    static bool point_in_triangle(const Vec2 &point, const std::array<Vec2, 3> &triangle)
    {
        const auto barycentric = triangle_barycentric(point, triangle);
        constexpr double eps = 1e-6;
        return barycentric[0] >= -eps && barycentric[1] >= -eps && barycentric[2] >= -eps;
    }

    static Vec2 closest_point_on_segment(const Vec2 &point, const Vec2 &start, const Vec2 &end)
    {
        const Vec2 edge = sub_vec(end, start);
        const double edge_len_sq = length_sq(edge);
        if (edge_len_sq <= 1e-9)
            return start;
        const double t = std::clamp(dot_vec(sub_vec(point, start), edge) / edge_len_sq, 0.0, 1.0);
        return add_vec(start, scale_vec(edge, t));
    }

    static Vec2 closest_point_on_triangle(const Vec2 &point, const std::array<Vec2, 3> &triangle)
    {
        if (point_in_triangle(point, triangle))
            return point;

        Vec2 best = triangle[0];
        double best_dist = std::numeric_limits<double>::max();
        for (int edge_idx = 0; edge_idx < 3; ++edge_idx) {
            const Vec2 candidate = closest_point_on_segment(point, triangle[edge_idx], triangle[(edge_idx + 1) % 3]);
            const double candidate_dist = dist_sq(point, candidate);
            if (candidate_dist < best_dist) {
                best_dist = candidate_dist;
                best = candidate;
            }
        }
        return best;
    }

    Vec2 normalized_point_from_mouse(const wxMouseEvent &evt) const
    {
        const wxRect rect = canvas_rect();
        const int width = std::max(1, rect.GetWidth() - 1);
        const int height = std::max(1, rect.GetHeight() - 1);
        return make_vec(
            std::clamp(double(evt.GetX() - rect.GetLeft()) / double(width), 0.0, 1.0),
            std::clamp(double(evt.GetY() - rect.GetTop()) / double(height), 0.0, 1.0));
    }

    Vec2 clamp_point_to_geometry(const Vec2 &point) const
    {
        switch (geometry_mode()) {
        case GeometryMode::Point:
            return make_vec(0.5, 0.5);
        case GeometryMode::Line:
            return make_vec(std::clamp(point.x, 0.0, 1.0), 0.5);
        case GeometryMode::Triangle:
        case GeometryMode::TriangleWithCenter:
            return closest_point_on_triangle(point, simplex_vertices());
        case GeometryMode::Radial:
            return make_vec(std::clamp(point.x, 0.0, 1.0), std::clamp(point.y, 0.0, 1.0));
        }
        return point;
    }

    std::vector<double> simplex_weights_from_pos(const Vec2 &point) const
    {
        const auto triangle = simplex_vertices();
        const Vec2 clamped = closest_point_on_triangle(point, triangle);
        const auto barycentric = triangle_barycentric(clamped, triangle);

        if (geometry_mode() == GeometryMode::Triangle)
            return { std::max(0.0, barycentric[0]), std::max(0.0, barycentric[1]), std::max(0.0, barycentric[2]) };

        const double shared = std::max(0.0, std::min({ barycentric[0], barycentric[1], barycentric[2] }));
        return {
            std::max(0.0, barycentric[0] - shared),
            std::max(0.0, barycentric[1] - shared),
            std::max(0.0, barycentric[2] - shared),
            std::max(0.0, shared * 3.0)
        };
    }

    Vec2 triangle_point_from_weights() const
    {
        const auto vertices = simplex_vertices();
        double total = 0.0;
        for (size_t idx = 0; idx < 3 && idx < m_weights.size(); ++idx)
            total += std::max(0, m_weights[idx]);
        if (total <= 0.0)
            return simplex_center();

        Vec2 out = make_vec(0.0, 0.0);
        for (size_t idx = 0; idx < 3 && idx < m_weights.size(); ++idx) {
            const double weight = double(std::max(0, m_weights[idx])) / total;
            out = add_vec(out, scale_vec(vertices[idx], weight));
        }
        return out;
    }

    void initialize_cursor_from_grid_search()
    {
        double best_x = 0.5;
        double best_y = 0.5;
        double best_error = std::numeric_limits<double>::max();
        constexpr int grid = 96;
        for (int y_idx = 0; y_idx <= grid; ++y_idx) {
            for (int x_idx = 0; x_idx <= grid; ++x_idx) {
                const Vec2 point = clamp_point_to_geometry(make_vec(double(x_idx) / double(grid), double(y_idx) / double(grid)));
                const std::vector<int> probe = normalized_weights_from_pos(point.x, point.y);
                if (probe.size() != m_weights.size())
                    continue;
                double error = 0.0;
                for (size_t idx = 0; idx < probe.size(); ++idx) {
                    const double delta = double(probe[idx] - m_weights[idx]);
                    error += delta * delta;
                }
                if (error < best_error) {
                    best_error = error;
                    best_x = point.x;
                    best_y = point.y;
                }
            }
        }
        m_cursor_x = best_x;
        m_cursor_y = best_y;
        m_weights = normalized_weights_from_pos(m_cursor_x, m_cursor_y);
    }

    std::vector<double> raw_weights_from_pos(double normalized_x, double normalized_y) const
    {
        switch (geometry_mode()) {
        case GeometryMode::Point:
            return { 1.0 };
        case GeometryMode::Line: {
            const double t = std::clamp(normalized_x, 0.0, 1.0);
            return { 1.0 - t, t };
        }
        case GeometryMode::Triangle:
        case GeometryMode::TriangleWithCenter:
            return simplex_weights_from_pos(make_vec(normalized_x, normalized_y));
        case GeometryMode::Radial:
            break;
        }

        const std::vector<AnchorPoint> anchors = radial_anchor_points();
        std::vector<double> out(anchors.size(), 0.0);
        if (anchors.empty())
            return out;

        constexpr double eps = 1e-8;
        size_t exact_idx = size_t(-1);
        for (size_t idx = 0; idx < anchors.size(); ++idx) {
            const double dx = normalized_x - anchors[idx].x;
            const double dy = normalized_y - anchors[idx].y;
            const double d2 = dx * dx + dy * dy;
            if (d2 <= eps) {
                exact_idx = idx;
                break;
            }
            out[idx] = 1.0 / std::max(1e-6, d2);
        }
        if (exact_idx != size_t(-1)) {
            std::fill(out.begin(), out.end(), 0.0);
            out[exact_idx] = 1.0;
            return out;
        }

        double sum = 0.0;
        for (const double value : out)
            sum += value;
        if (sum <= 0.0) {
            out.assign(out.size(), 0.0);
            out[0] = 1.0;
            return out;
        }
        for (double &value : out)
            value /= sum;
        return out;
    }

    std::vector<int> normalized_weights_from_pos(double normalized_x, double normalized_y) const
    {
        std::vector<int> raw_weights;
        const std::vector<double> raw = raw_weights_from_pos(normalized_x, normalized_y);
        raw_weights.reserve(raw.size());
        for (const double value : raw)
            raw_weights.emplace_back(std::max(0, int(std::lround(value * 100.0))));
        return normalize_color_match_weights(raw_weights, raw.size());
    }

    void initialize_cursor_from_weights()
    {
        if (m_weights.empty()) {
            m_cursor_x = 0.5;
            m_cursor_y = 0.5;
            return;
        }

        switch (geometry_mode()) {
        case GeometryMode::Point:
            m_cursor_x = 0.5;
            m_cursor_y = 0.5;
            break;
        case GeometryMode::Line: {
            const int total = std::accumulate(m_weights.begin(), m_weights.end(), 0);
            const double t = total > 0 && m_weights.size() >= 2 ? double(std::max(0, m_weights[1])) / double(total) : 0.5;
            m_cursor_x = std::clamp(t, 0.0, 1.0);
            m_cursor_y = 0.5;
            m_weights = normalized_weights_from_pos(m_cursor_x, m_cursor_y);
            break;
        }
        case GeometryMode::Triangle: {
            const Vec2 point = triangle_point_from_weights();
            m_cursor_x = point.x;
            m_cursor_y = point.y;
            m_weights = normalized_weights_from_pos(m_cursor_x, m_cursor_y);
            break;
        }
        case GeometryMode::TriangleWithCenter:
        case GeometryMode::Radial:
            initialize_cursor_from_grid_search();
            break;
        }
    }

    void emit_changed()
    {
        wxCommandEvent evt(wxEVT_SLIDER, GetId());
        evt.SetEventObject(this);
        ProcessWindowEvent(evt);
    }

    void update_from_mouse(const wxMouseEvent &evt, bool notify)
    {
        const Vec2 point = clamp_point_to_geometry(normalized_point_from_mouse(evt));
        m_cursor_x = point.x;
        m_cursor_y = point.y;
        m_weights = normalized_weights_from_pos(m_cursor_x, m_cursor_y);
        Refresh();
        if (notify)
            emit_changed();
    }

    wxColour canvas_background_color() const
    {
        return GetBackgroundColour().IsOk() ? GetBackgroundColour() : wxColour(245, 245, 245);
    }

    bool cached_bitmap_matches(const wxSize &size, const wxColour &background) const
    {
        return m_cached_bitmap.IsOk() && m_cached_bitmap_size == size && m_cached_background == background;
    }

    void schedule_cached_bitmap_render()
    {
        if (!m_render_timer.IsRunning())
            m_render_timer.StartOnce(80);
    }

    void invalidate_cached_bitmap()
    {
        m_cached_bitmap = wxBitmap();
        m_cached_bitmap_size = wxSize();
        m_cached_background = wxColour();
    }

    void render_cached_bitmap(const wxSize &size, const wxColour &background)
    {
        const int width = size.GetWidth();
        const int height = size.GetHeight();
        if (width <= 0 || height <= 0)
            return;

        wxImage image(width, height);
        unsigned char *data = image.GetData();
        if (data != nullptr) {
            for (int y = 0; y < height; ++y) {
                const double normalized_y = (height > 1) ? double(y) / double(height - 1) : 0.5;
                for (int x = 0; x < width; ++x) {
                    const double normalized_x = (width > 1) ? double(x) / double(width - 1) : 0.5;
                    const int data_idx = (y * width + x) * 3;
                    bool paint_pixel = true;
                    if (geometry_mode() == GeometryMode::Triangle || geometry_mode() == GeometryMode::TriangleWithCenter)
                        paint_pixel = point_in_triangle(make_vec(normalized_x, normalized_y), simplex_vertices());

                    const std::vector<double> raw_weights = raw_weights_from_pos(normalized_x, normalized_y);
                    wxColour color = paint_pixel ? blend_multi_filament_mixer(m_colors, raw_weights) : background;
                    if (paint_pixel && m_min_component_percent > 0 &&
                        !color_match_raw_weights_within_range(raw_weights, m_min_component_percent)) {
                        const bool stripe = (((x + y) / 8) % 2) == 0;
                        const double factor = stripe ? 0.12 : 0.38;
                        color = wxColour(
                            static_cast<unsigned char>(std::clamp(int(std::lround(double(color.Red()) * factor)), 0, 255)),
                            static_cast<unsigned char>(std::clamp(int(std::lround(double(color.Green()) * factor)), 0, 255)),
                            static_cast<unsigned char>(std::clamp(int(std::lround(double(color.Blue()) * factor)), 0, 255)));
                    }
                    data[data_idx + 0] = color.Red();
                    data[data_idx + 1] = color.Green();
                    data[data_idx + 2] = color.Blue();
                }
            }
        }

        m_cached_bitmap = wxBitmap(image);
        m_cached_bitmap_size = size;
        m_cached_background = background;
    }

    void draw_cached_bitmap(wxAutoBufferedPaintDC &dc, const wxRect &rect)
    {
        if (!m_cached_bitmap.IsOk())
            return;

        if (m_cached_bitmap_size == rect.GetSize()) {
            dc.DrawBitmap(m_cached_bitmap, rect.GetLeft(), rect.GetTop(), false);
            return;
        }

        wxMemoryDC memdc;
        memdc.SelectObject(m_cached_bitmap);
        dc.StretchBlit(rect.GetLeft(), rect.GetTop(), rect.GetWidth(), rect.GetHeight(),
                       &memdc, 0, 0, m_cached_bitmap_size.GetWidth(), m_cached_bitmap_size.GetHeight());
        memdc.SelectObject(wxNullBitmap);
    }

    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();

        const wxRect rect = canvas_rect();
        const int width = rect.GetWidth();
        const int height = rect.GetHeight();
        if (width <= 0 || height <= 0)
            return;

        const wxColour background = canvas_background_color();
        if (!cached_bitmap_matches(rect.GetSize(), background)) {
            if (!m_cached_bitmap.IsOk())
                render_cached_bitmap(rect.GetSize(), background);
            else
                schedule_cached_bitmap_render();
        }
        draw_cached_bitmap(dc, rect);

        if (geometry_mode() == GeometryMode::Triangle || geometry_mode() == GeometryMode::TriangleWithCenter) {
            const auto triangle = simplex_vertices();
            wxPoint points[3] = {
                wxPoint(rect.GetLeft() + int(std::lround(triangle[0].x * double(std::max(1, width - 1)))),
                        rect.GetTop()  + int(std::lround(triangle[0].y * double(std::max(1, height - 1))))),
                wxPoint(rect.GetLeft() + int(std::lround(triangle[1].x * double(std::max(1, width - 1)))),
                        rect.GetTop()  + int(std::lround(triangle[1].y * double(std::max(1, height - 1))))),
                wxPoint(rect.GetLeft() + int(std::lround(triangle[2].x * double(std::max(1, width - 1)))),
                        rect.GetTop()  + int(std::lround(triangle[2].y * double(std::max(1, height - 1)))))
            };
            dc.SetPen(wxPen(wxColour(160, 160, 160), 1));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawPolygon(3, points);
            if (geometry_mode() == GeometryMode::TriangleWithCenter) {
                const Vec2 center = simplex_center();
                const wxPoint center_pt(rect.GetLeft() + int(std::lround(center.x * double(std::max(1, width - 1)))),
                                        rect.GetTop()  + int(std::lround(center.y * double(std::max(1, height - 1)))));
                dc.SetPen(wxPen(wxColour(180, 180, 180), 1, wxPENSTYLE_DOT));
                for (const wxPoint &vertex : points)
                    dc.DrawLine(center_pt, vertex);
            }
        } else {
            dc.SetPen(wxPen(wxColour(160, 160, 160), 1));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRectangle(rect);
        }

        dc.SetPen(wxPen(wxColour(160, 160, 160), 1));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);

        const auto anchors = anchor_points();
        for (size_t idx = 0; idx < anchors.size() && idx < m_colors.size(); ++idx) {
            const int anchor_x = rect.GetLeft() + int(std::lround(anchors[idx].x * double(std::max(1, width - 1))));
            const int anchor_y = rect.GetTop() + int(std::lround(anchors[idx].y * double(std::max(1, height - 1))));
            dc.SetPen(wxPen(wxColour(30, 30, 30), 1));
            dc.SetBrush(wxBrush(m_colors[idx]));
            dc.DrawCircle(wxPoint(anchor_x, anchor_y), FromDIP(4));
        }

        const int cursor_x = rect.GetLeft() + int(std::lround(m_cursor_x * double(std::max(1, width - 1))));
        const int cursor_y = rect.GetTop() + int(std::lround(m_cursor_y * double(std::max(1, height - 1))));
        dc.SetPen(wxPen(wxColour(255, 255, 255), 3));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawCircle(wxPoint(cursor_x, cursor_y), FromDIP(7));
        dc.SetPen(wxPen(wxColour(30, 30, 30), 1));
        dc.DrawCircle(wxPoint(cursor_x, cursor_y), FromDIP(7));
    }

    void on_left_down(wxMouseEvent &evt)
    {
        if (!HasCapture())
            CaptureMouse();
        m_dragging = true;
        update_from_mouse(evt, true);
    }

    void on_left_up(wxMouseEvent &evt)
    {
        if (m_dragging)
            update_from_mouse(evt, true);
        m_dragging = false;
        if (HasCapture())
            ReleaseMouse();
    }

    void on_mouse_move(wxMouseEvent &evt)
    {
        if (m_dragging && evt.LeftIsDown())
            update_from_mouse(evt, true);
    }

    void on_capture_lost(wxMouseCaptureLostEvent &)
    {
        m_dragging = false;
    }

    void on_size(wxSizeEvent &evt)
    {
        if (m_cached_bitmap.IsOk())
            schedule_cached_bitmap_render();
        Refresh(false);
        evt.Skip();
    }

    void on_render_timer(wxTimerEvent &)
    {
        const wxRect rect = canvas_rect();
        render_cached_bitmap(rect.GetSize(), canvas_background_color());
        Refresh(false);
    }

private:
    std::vector<wxColour>     m_colors;
    std::vector<int>          m_weights;
    wxBitmap                  m_cached_bitmap;
    wxSize                    m_cached_bitmap_size;
    wxColour                  m_cached_background;
    wxTimer                   m_render_timer;
    int                       m_min_component_percent { 0 };
    double                    m_cursor_x { 0.5 };
    double                    m_cursor_y { 0.5 };
    bool                      m_dragging { false };
};

class MixedFilamentColorMatchDialog : public DPIDialog
{
public:
    MixedFilamentColorMatchDialog(wxWindow *parent,
                                  const std::vector<std::string> &physical_colors,
                                  const wxColour &initial_color)
        : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                    wxID_ANY,
                    _L("Add Color"),
                    wxDefaultPosition,
                    wxDefaultSize,
                    wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_physical_colors(physical_colors)
    {
        m_recipe_timer.SetOwner(this);
        m_loading_timer.SetOwner(this);

        m_palette.reserve(m_physical_colors.size());
        for (const std::string &hex : m_physical_colors)
            m_palette.emplace_back(parse_mixed_color(hex));

        const wxColour safe_initial = initial_color.IsOk() ? initial_color :
            (m_palette.size() >= 2 ? blend_pair_filament_mixer(m_palette[0], m_palette[1], 0.5f) : wxColour("#26A69A"));
        std::vector<int> initial_weights(m_palette.size(), 0);
        if (!initial_weights.empty())
            initial_weights[0] = 100;
        if (initial_weights.size() >= 2) {
            initial_weights[0] = 50;
            initial_weights[1] = 50;
        }

        std::vector<unsigned int> filament_ids;
        filament_ids.reserve(m_palette.size());
        for (size_t idx = 0; idx < m_palette.size(); ++idx)
            filament_ids.emplace_back(unsigned(idx + 1));

        SetMinSize(wxSize(FromDIP(430), FromDIP(520)));

        auto *root = new wxBoxSizer(wxVERTICAL);
        auto *description = new wxStaticText(
            this, wxID_ANY,
            _L("Pick from the current filament gamut. The dialog previews the closest 2-color, 3-color, or 4-color FilamentMixer recipe before it is added."));
        description->Wrap(FromDIP(390));
        root->Add(description, 0, wxEXPAND | wxALL, FromDIP(12));

        m_color_map = new MixedFilamentColorMapPanel(this, filament_ids, m_palette, initial_weights,
                                                     wxSize(FromDIP(260), FromDIP(260)));
        root->Add(m_color_map, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

        auto *hex_row = new wxBoxSizer(wxHORIZONTAL);
        hex_row->Add(new wxStaticText(this, wxID_ANY, _L("Hex")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        m_hex_input = new wxTextCtrl(this, wxID_ANY, normalize_color_match_hex(safe_initial.GetAsString(wxC2S_HTML_SYNTAX)),
                                     wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
        m_hex_input->SetToolTip(_L("Enter a hex color like #00FF88. The picker will snap to the closest supported FilamentMixer color."));
        hex_row->Add(m_hex_input, 1, wxALIGN_CENTER_VERTICAL);
        hex_row->AddSpacer(FromDIP(8));
        m_classic_picker = new wxColourPickerCtrl(this, wxID_ANY, safe_initial);
        m_classic_picker->SetToolTip(_L("Classic color picker. The result will snap to the closest supported FilamentMixer color."));
        hex_row->Add(m_classic_picker, 0, wxALIGN_CENTER_VERTICAL);
        root->Add(hex_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

        auto *range_row = new wxBoxSizer(wxHORIZONTAL);
        range_row->Add(new wxStaticText(this, wxID_ANY, _L("Range")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        m_range_slider = new wxSlider(this, wxID_ANY, m_min_component_percent, 0, 50);
        m_range_slider->SetToolTip(_L("Minimum percent for each participating color. Higher values block highly skewed mixes."));
        range_row->Add(m_range_slider, 1, wxALIGN_CENTER_VERTICAL);
        range_row->AddSpacer(FromDIP(8));
        m_range_value = new wxStaticText(this, wxID_ANY, wxEmptyString);
        range_row->Add(m_range_value, 0, wxALIGN_CENTER_VERTICAL);
        root->Add(range_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

        auto *summary_grid = new wxFlexGridSizer(2, FromDIP(8), FromDIP(8));
        summary_grid->AddGrowableCol(1, 1);

        summary_grid->Add(new wxStaticText(this, wxID_ANY, _L("Requested")), 0, wxALIGN_CENTER_VERTICAL);
        auto *selected_row = new wxBoxSizer(wxHORIZONTAL);
        m_selected_preview = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(72), FromDIP(24)), wxBORDER_SIMPLE);
        selected_row->Add(m_selected_preview, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        m_selected_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
        selected_row->Add(m_selected_label, 1, wxALIGN_CENTER_VERTICAL);
        summary_grid->Add(selected_row, 1, wxEXPAND);

        summary_grid->Add(new wxStaticText(this, wxID_ANY, _L("Creates")), 0, wxALIGN_CENTER_VERTICAL);
        auto *recipe_row = new wxBoxSizer(wxHORIZONTAL);
        m_recipe_preview = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(72), FromDIP(24)), wxBORDER_SIMPLE);
        recipe_row->Add(m_recipe_preview, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        m_recipe_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
        m_recipe_label->Wrap(FromDIP(280));
        recipe_row->Add(m_recipe_label, 1, wxALIGN_CENTER_VERTICAL);
        summary_grid->Add(recipe_row, 1, wxEXPAND);

        root->Add(summary_grid, 0, wxEXPAND | wxALL, FromDIP(12));

        m_delta_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
        root->Add(m_delta_label, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

        m_presets_label = new wxStaticText(this, wxID_ANY, _L("Exact preset mixes"));
        root->Add(m_presets_label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
        m_presets_host = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(96)),
                                              wxVSCROLL | wxBORDER_SIMPLE);
        m_presets_host->SetScrollRate(FromDIP(6), FromDIP(6));
        m_presets_sizer = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);
        m_presets_host->SetSizer(m_presets_sizer);
        root->Add(m_presets_host, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

        m_error_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
        m_error_label->SetForegroundColour(wxColour(196, 67, 63));
        root->Add(m_error_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

        if (wxSizer *button_sizer = CreateStdDialogButtonSizer(wxOK | wxCANCEL))
            root->Add(button_sizer, 0, wxEXPAND | wxALL, FromDIP(12));

        m_loading_panel = new wxPanel(this, wxID_ANY);
        m_loading_panel->SetMinSize(wxSize(-1, FromDIP(24)));
        auto *loading_row = new wxBoxSizer(wxHORIZONTAL);
        m_loading_label = new wxStaticText(m_loading_panel, wxID_ANY, " ");
        loading_row->Add(m_loading_label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        m_loading_gauge = new wxGauge(m_loading_panel, wxID_ANY, 100, wxDefaultPosition, wxSize(FromDIP(150), FromDIP(8)),
                                      wxGA_HORIZONTAL | wxGA_SMOOTH);
        m_loading_gauge->SetValue(0);
        m_loading_gauge->Enable(false);
        loading_row->Add(m_loading_gauge, 0, wxALIGN_CENTER_VERTICAL);
        m_loading_panel->SetSizer(loading_row);
        root->Add(m_loading_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        SetSizerAndFit(root);

        m_selected_target = safe_initial;
        m_requested_target = safe_initial;
        if (m_color_map)
            m_color_map->set_min_component_percent(m_min_component_percent);
        update_range_label();
        rebuild_presets_ui();
        sync_inputs_to_requested();
        update_dialog_state();

        if (m_color_map) {
            m_color_map->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
                if (!m_color_map)
                    return;
                request_recipe_match(m_color_map->selected_color(), true, _L("Matching closest supported mix..."));
            });
        }

        if (m_hex_input) {
            m_hex_input->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &) {
                apply_hex_input(true);
            });
            m_hex_input->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &evt) {
                apply_hex_input(false);
                evt.Skip();
            });
        }
        if (m_classic_picker) {
            m_classic_picker->Bind(wxEVT_COLOURPICKER_CHANGED, [this](wxColourPickerEvent &evt) {
                if (m_syncing_inputs)
                    return;
                apply_requested_target(evt.GetColour());
            });
        }
        if (m_range_slider) {
            m_range_slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
                m_min_component_percent = m_range_slider ? std::clamp(m_range_slider->GetValue(), 0, 50) : m_min_component_percent;
                update_range_label();
                if (m_color_map)
                    m_color_map->set_min_component_percent(m_min_component_percent);
                rebuild_presets_ui();
                request_recipe_match(m_requested_target, true, _L("Matching closest supported mix..."));
            });
        }

        Bind(wxEVT_TIMER, [this](wxTimerEvent &) { refresh_selected_recipe(); }, m_recipe_timer.GetId());
        Bind(wxEVT_TIMER, [this](wxTimerEvent &) {
            if (m_loading_gauge && m_recipe_loading)
                m_loading_gauge->Pulse();
        }, m_loading_timer.GetId());
        if (wxWindow *ok_button = FindWindow(wxID_OK)) {
            ok_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &evt) {
                if (m_recipe_refresh_pending)
                    refresh_selected_recipe();
                if (m_recipe_loading || !m_selected_recipe.valid)
                    return;
                evt.Skip();
            });
        }

        CentreOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
    }

    ~MixedFilamentColorMatchDialog() override
    {
        if (m_recipe_timer.IsRunning())
            m_recipe_timer.Stop();
        if (m_loading_timer.IsRunning())
            m_loading_timer.Stop();
    }

    void begin_initial_recipe_load()
    {
        request_recipe_match(m_requested_target, false, _L("Calculating closest supported mix..."));
    }

    MixedColorMatchRecipeResult selected_recipe() const { return m_selected_recipe; }

    void on_dpi_changed(const wxRect &suggested_rect) override
    {
        wxUnusedVar(suggested_rect);
        Layout();
        Fit();
        Refresh();
    }

private:
    void update_range_label()
    {
        if (m_range_value)
            m_range_value->SetLabel(wxString::Format(_L("%d%% min"), m_min_component_percent));
    }

    void rebuild_presets_ui()
    {
        if (!m_presets_host || !m_presets_sizer || !m_presets_label)
            return;

        m_presets = build_color_match_presets(m_physical_colors, m_min_component_percent);

        m_presets_host->Freeze();
        while (m_presets_sizer->GetItemCount() > 0) {
            wxSizerItem *item = m_presets_sizer->GetItem(size_t(0));
            wxWindow *window = item ? item->GetWindow() : nullptr;
            m_presets_sizer->Remove(0);
            if (window)
                window->Destroy();
        }

        for (const MixedColorMatchRecipeResult &preset : m_presets) {
            auto *button = new wxBitmapButton(m_presets_host, wxID_ANY,
                                              make_color_match_swatch_bitmap(preset.preview_color, wxSize(FromDIP(30), FromDIP(20))),
                                              wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
            const wxString tooltip = from_u8(summarize_color_match_recipe(preset)) + "\n" +
                normalize_color_match_hex(preset.preview_color.GetAsString(wxC2S_HTML_SYNTAX));
            button->SetToolTip(tooltip);
            button->Bind(wxEVT_BUTTON, [this, preset](wxCommandEvent &) { apply_preset(preset); });
            m_presets_sizer->Add(button, 0, wxALL, FromDIP(2));
        }

        m_presets_host->FitInside();
        const bool show_presets = !m_presets.empty();
        m_presets_label->Show(show_presets);
        m_presets_host->Show(show_presets);
        m_presets_host->Thaw();
    }

    void set_recipe_loading(bool loading, const wxString &message)
    {
        m_recipe_loading = loading;
        if (!message.empty())
            m_loading_message = message;

        if (m_loading_label)
            m_loading_label->SetLabel(loading ? m_loading_message : wxString(" "));
        if (m_loading_gauge) {
            if (loading) {
                m_loading_gauge->Enable(true);
                m_loading_gauge->Pulse();
                if (!m_loading_timer.IsRunning())
                    m_loading_timer.Start(100);
            } else {
                if (m_loading_timer.IsRunning())
                    m_loading_timer.Stop();
                m_loading_gauge->SetValue(0);
                m_loading_gauge->Enable(false);
            }
        }
    }

    void sync_inputs_to_requested()
    {
        m_syncing_inputs = true;
        if (m_hex_input)
            m_hex_input->ChangeValue(normalize_color_match_hex(m_requested_target.GetAsString(wxC2S_HTML_SYNTAX)));
        if (m_classic_picker)
            m_classic_picker->SetColour(m_requested_target);
        m_syncing_inputs = false;
    }

    bool apply_requested_target(const wxColour &requested_target)
    {
        request_recipe_match(requested_target, false, _L("Matching closest supported mix..."));
        return true;
    }

    bool apply_hex_input(bool show_invalid_error)
    {
        if (!m_hex_input || m_syncing_inputs)
            return false;

        wxColour parsed;
        if (!try_parse_color_match_hex(m_hex_input->GetValue(), parsed)) {
            if (show_invalid_error && m_error_label)
                m_error_label->SetLabel(_L("Use a valid hex color like #00FF88."));
            return false;
        }

        return apply_requested_target(parsed);
    }

    void request_recipe_match(const wxColour &requested_target, bool debounce, const wxString &loading_message)
    {
        m_requested_target = requested_target;
        m_selected_target = requested_target;
        sync_inputs_to_requested();

        ++m_recipe_request_token;
        set_recipe_loading(true, loading_message);

        if (m_recipe_timer.IsRunning())
            m_recipe_timer.Stop();
        m_recipe_refresh_pending = debounce;
        update_dialog_state();

        if (debounce) {
            m_recipe_timer.StartOnce(120);
            return;
        }

        launch_recipe_match(m_recipe_request_token, requested_target);
    }

    void refresh_selected_recipe()
    {
        m_recipe_refresh_pending = false;
        launch_recipe_match(m_recipe_request_token, m_requested_target);
    }

    void launch_recipe_match(size_t request_token, const wxColour &requested_target)
    {
        const std::vector<std::string> physical_colors = m_physical_colors;
        const int min_component_percent = m_min_component_percent;
        wxWeakRef<wxWindow> weak_self(this);
        std::thread([weak_self, physical_colors, requested_target, request_token, min_component_percent]() {
            MixedColorMatchRecipeResult recipe = build_best_color_match_recipe(physical_colors, requested_target, min_component_percent);
            wxGetApp().CallAfter([weak_self, requested_target, recipe = std::move(recipe), request_token]() mutable {
                if (!weak_self)
                    return;
                auto *self = static_cast<MixedFilamentColorMatchDialog *>(weak_self.get());
                self->handle_recipe_result(request_token, requested_target, std::move(recipe));
            });
        }).detach();
    }

    void handle_recipe_result(size_t request_token, const wxColour &requested_target, MixedColorMatchRecipeResult recipe)
    {
        if (request_token != m_recipe_request_token)
            return;

        m_has_recipe_result = true;
        m_selected_recipe = std::move(recipe);
        set_recipe_loading(false, wxEmptyString);

        if (m_selected_recipe.valid) {
            m_selected_target = m_selected_recipe.preview_color;
            if (m_color_map)
                m_color_map->set_normalized_weights(expand_color_match_recipe_weights(m_selected_recipe, m_palette.size()), false);
            sync_inputs_to_requested();
        } else {
            m_selected_target = requested_target;
        }

        update_dialog_state();
    }

    void apply_preset(MixedColorMatchRecipeResult preset)
    {
        preset.delta_e = 0.0;
        ++m_recipe_request_token;
        m_requested_target = preset.preview_color;
        m_selected_target = preset.preview_color;
        m_selected_recipe = std::move(preset);
        m_has_recipe_result = true;
        m_recipe_refresh_pending = false;
        if (m_recipe_timer.IsRunning())
            m_recipe_timer.Stop();
        set_recipe_loading(false, wxEmptyString);
        if (m_color_map)
            m_color_map->set_normalized_weights(expand_color_match_recipe_weights(m_selected_recipe, m_palette.size()), false);
        sync_inputs_to_requested();
        update_dialog_state();
    }

    void update_dialog_state()
    {
        const wxColour fallback = wxColour("#26A69A");
        if (m_selected_preview) {
            m_selected_preview->SetBackgroundColour(m_requested_target.IsOk() ? m_requested_target : fallback);
            m_selected_preview->Refresh();
        }
        if (m_selected_label)
            m_selected_label->SetLabel(m_requested_target.IsOk() ?
                normalize_color_match_hex(m_requested_target.GetAsString(wxC2S_HTML_SYNTAX)) :
                normalize_color_match_hex(fallback.GetAsString(wxC2S_HTML_SYNTAX)));

        const bool valid = m_selected_recipe.valid;
        const wxColour recipe_color = (valid && m_selected_recipe.preview_color.IsOk()) ?
            m_selected_recipe.preview_color :
            (m_requested_target.IsOk() ? m_requested_target : fallback);
        if (m_recipe_preview) {
            m_recipe_preview->SetBackgroundColour(recipe_color);
            m_recipe_preview->Refresh();
        }
        if (m_recipe_label) {
            if (m_recipe_loading) {
                m_recipe_label->SetLabel(m_loading_message);
            } else if (valid) {
                const wxString recipe_summary = from_u8(summarize_color_match_recipe(m_selected_recipe));
                const wxString recipe_hex = normalize_color_match_hex(recipe_color.GetAsString(wxC2S_HTML_SYNTAX));
                m_recipe_label->SetLabel(recipe_summary + "  " + recipe_hex);
            } else if (m_has_recipe_result) {
                m_recipe_label->SetLabel(_L("No supported 2-color, 3-color, or 4-color recipe found."));
            } else {
                m_recipe_label->SetLabel(wxEmptyString);
            }
        }
        if (m_delta_label) {
            if (m_recipe_loading && m_requested_target.IsOk()) {
                m_delta_label->SetLabel(wxString::Format(_L("Matching %s..."),
                                                         normalize_color_match_hex(m_requested_target.GetAsString(wxC2S_HTML_SYNTAX))));
            } else if (valid && m_requested_target.IsOk()) {
                m_delta_label->SetLabel(wxString::Format(_L("Requested %s, closest recipe delta: %.2f"),
                                                         normalize_color_match_hex(m_requested_target.GetAsString(wxC2S_HTML_SYNTAX)),
                                                         m_selected_recipe.delta_e));
            } else {
                m_delta_label->SetLabel(wxEmptyString);
            }
        }
        if (m_error_label) {
            if (m_recipe_loading)
                m_error_label->SetLabel(wxEmptyString);
            else if (!valid && m_has_recipe_result)
                m_error_label->SetLabel(_L("Unable to create a color mix from the current physical filament colors within the selected range."));
            else if (m_hex_input && !m_syncing_inputs) {
                wxColour parsed;
                if (!try_parse_color_match_hex(m_hex_input->GetValue(), parsed))
                    m_error_label->SetLabel(_L("Use a valid hex color like #00FF88."));
                else
                    m_error_label->SetLabel(wxEmptyString);
            } else {
                m_error_label->SetLabel(wxEmptyString);
            }
        }
        if (wxWindow *ok_button = FindWindow(wxID_OK))
            ok_button->Enable(valid && !m_recipe_loading && !m_recipe_refresh_pending);

        Layout();
    }

private:
    std::vector<std::string>                m_physical_colors;
    std::vector<wxColour>                   m_palette;
    std::vector<MixedColorMatchRecipeResult> m_presets;
    MixedFilamentColorMapPanel             *m_color_map        = nullptr;
    wxTextCtrl                             *m_hex_input        = nullptr;
    wxColourPickerCtrl                     *m_classic_picker   = nullptr;
    wxSlider                               *m_range_slider     = nullptr;
    wxStaticText                           *m_range_value      = nullptr;
    wxStaticText                           *m_presets_label    = nullptr;
    wxScrolledWindow                       *m_presets_host     = nullptr;
    wxWrapSizer                            *m_presets_sizer    = nullptr;
    wxPanel                                *m_loading_panel    = nullptr;
    wxStaticText                           *m_loading_label    = nullptr;
    wxGauge                                *m_loading_gauge    = nullptr;
    wxPanel                                *m_selected_preview = nullptr;
    wxStaticText                           *m_selected_label   = nullptr;
    wxPanel                                *m_recipe_preview   = nullptr;
    wxStaticText                           *m_recipe_label     = nullptr;
    wxStaticText                           *m_delta_label      = nullptr;
    wxStaticText                           *m_error_label      = nullptr;
    wxColour                                m_requested_target { wxColour("#26A69A") };
    wxColour                                m_selected_target { wxColour("#26A69A") };
    MixedColorMatchRecipeResult             m_selected_recipe;
    wxTimer                                 m_recipe_timer;
    wxTimer                                 m_loading_timer;
    wxString                                m_loading_message;
    size_t                                  m_recipe_request_token { 0 };
    int                                     m_min_component_percent { 15 };
    bool                                    m_has_recipe_result { false };
    bool                                    m_recipe_loading { false };
    bool                                    m_recipe_refresh_pending { false };
    bool                                    m_syncing_inputs { false };
};

class MixedGradientSelector : public wxPanel
{
public:
    MixedGradientSelector(wxWindow *parent, const wxColour &left, const wxColour &right, int value_percent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
        , m_left(left)
        , m_right(right)
        , m_value(std::clamp(value_percent, 0, 100))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(FromDIP(96), FromDIP(12)));
        Bind(wxEVT_PAINT, &MixedGradientSelector::on_paint, this);
        Bind(wxEVT_LEFT_DOWN, &MixedGradientSelector::on_left_down, this);
        Bind(wxEVT_LEFT_UP, &MixedGradientSelector::on_left_up, this);
        Bind(wxEVT_MOTION, &MixedGradientSelector::on_mouse_move, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, &MixedGradientSelector::on_capture_lost, this);
    }

    ~MixedGradientSelector() override
    {
        if (HasCapture())
            ReleaseMouse();
    }

    int value() const { return m_value; }
    bool is_multi_mode() const { return m_multi_mode; }

    void set_colors(const wxColour &left, const wxColour &right)
    {
        m_left = left;
        m_right = right;
        m_multi_mode = false;
        m_multi_colors.clear();
        m_multi_weights.clear();
        Refresh();
    }

    void set_multi_preview(const std::vector<wxColour> &corner_colors, const std::vector<int> &weights)
    {
        m_multi_mode = corner_colors.size() >= 3;
        m_multi_colors = corner_colors;
        m_multi_weights = weights;
        Refresh();
    }

private:
    wxRect gradient_rect() const
    {
        const int margin_x = FromDIP(2);
        const int margin_y = FromDIP(1);
        const wxSize sz = GetClientSize();
        return wxRect(margin_x, margin_y, std::max(1, sz.GetWidth() - margin_x * 2), std::max(1, sz.GetHeight() - margin_y * 2));
    }

    int value_from_x(int x) const
    {
        const wxRect rect = gradient_rect();
        const int min_x = rect.GetLeft();
        const int max_x = rect.GetLeft() + rect.GetWidth();
        const int clamped_x = std::clamp(x, min_x, max_x);
        return ((clamped_x - min_x) * 100 + rect.GetWidth() / 2) / rect.GetWidth();
    }

    void update_from_x(int x, bool notify)
    {
        const int new_value = value_from_x(x);
        m_value = new_value;
        Refresh();

        if (notify) {
            wxCommandEvent evt(wxEVT_SLIDER, GetId());
            evt.SetInt(m_value);
            evt.SetEventObject(this);
            ProcessWindowEvent(evt);
        }
    }

    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();
        const bool is_dark = wxGetApp().dark_mode();

        const wxRect rect = gradient_rect();
        if (m_multi_mode && m_multi_colors.size() >= 3) {
            const wxPoint tl(rect.GetLeft(), rect.GetTop());
            const wxPoint tr(rect.GetRight(), rect.GetTop());
            const wxPoint br(rect.GetRight(), rect.GetBottom());
            const wxPoint bl(rect.GetLeft(), rect.GetBottom());
            const wxPoint cc(rect.GetLeft() + rect.GetWidth() / 2, rect.GetTop() + rect.GetHeight() / 2);
            auto draw_tri = [&dc](const wxColour &color, const wxPoint &a, const wxPoint &b, const wxPoint &c) {
                wxPoint pts[3] = { a, b, c };
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxBrush(color));
                dc.DrawPolygon(3, pts);
            };

            if (m_multi_colors.size() >= 4) {
                draw_tri(m_multi_colors[0], tl, tr, cc);
                draw_tri(m_multi_colors[1], tr, br, cc);
                draw_tri(m_multi_colors[2], br, bl, cc);
                draw_tri(m_multi_colors[3], bl, tl, cc);
            } else {
                // 3-color layout: first color occupies one full side, two others on the opposite corners.
                draw_tri(m_multi_colors[0], tl, bl, cc);
                draw_tri(m_multi_colors[1], tl, tr, cc);
                draw_tri(m_multi_colors[2], bl, br, cc);
            }

            if (m_multi_weights.size() == m_multi_colors.size()) {
                dc.SetTextForeground(is_dark ? wxColour(236, 236, 236) : wxColour(20, 20, 20));
                dc.SetFont(Label::Body_10);
                const int pad = FromDIP(2);
                if (m_multi_colors.size() >= 4) {
                    dc.DrawText(wxString::Format("%d%%", m_multi_weights[0]), rect.GetLeft() + pad, rect.GetTop() + pad);
                    dc.DrawText(wxString::Format("%d%%", m_multi_weights[1]), rect.GetRight() - FromDIP(28), rect.GetTop() + pad);
                    dc.DrawText(wxString::Format("%d%%", m_multi_weights[2]), rect.GetRight() - FromDIP(28), rect.GetBottom() - FromDIP(14));
                    dc.DrawText(wxString::Format("%d%%", m_multi_weights[3]), rect.GetLeft() + pad, rect.GetBottom() - FromDIP(14));
                } else {
                    dc.DrawText(wxString::Format("%d%%", m_multi_weights[0]), rect.GetLeft() + pad, rect.GetTop() + rect.GetHeight() / 2 - FromDIP(6));
                    dc.DrawText(wxString::Format("%d%%", m_multi_weights[1]), rect.GetRight() - FromDIP(28), rect.GetTop() + pad);
                    dc.DrawText(wxString::Format("%d%%", m_multi_weights[2]), rect.GetRight() - FromDIP(28), rect.GetBottom() - FromDIP(14));
                }
            }
        } else {
            const int w = rect.GetWidth();
            const int h = rect.GetHeight();
            wxImage img(w, h);
            unsigned char *data = img.GetData();
            if (data != nullptr) {
                for (int x = 0; x < w; ++x) {
                    const float t = (w > 1) ? float(x) / float(w - 1) : 0.5f;
                    const wxColour col = blend_pair_filament_mixer(m_left, m_right, t);
                    const unsigned char r = static_cast<unsigned char>(col.Red());
                    const unsigned char g = static_cast<unsigned char>(col.Green());
                    const unsigned char b = static_cast<unsigned char>(col.Blue());
                    for (int y = 0; y < h; ++y) {
                        const int idx = (y * w + x) * 3;
                        data[idx + 0] = r;
                        data[idx + 1] = g;
                        data[idx + 2] = b;
                    }
                }
                dc.DrawBitmap(wxBitmap(img), rect.GetLeft(), rect.GetTop(), false);
            } else {
                dc.GradientFillLinear(rect, m_left, m_right, wxEAST);
            }
        }
        dc.SetPen(wxPen(is_dark ? wxColour(100, 100, 106) : wxColour(170, 170, 170), 1));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(rect);

        if (m_multi_mode) {
            dc.SetTextForeground(is_dark ? wxColour(236, 236, 236) : wxColour(30, 30, 30));
            dc.SetFont(Label::Body_10);
            const wxString hint = _L("Click to edit");
            wxSize text_sz = dc.GetTextExtent(hint);
            dc.DrawText(hint, rect.GetRight() - text_sz.GetWidth() - FromDIP(4), rect.GetTop() + FromDIP(2));
            return;
        }

        int marker_x = rect.GetLeft() + (rect.GetWidth() * m_value + 50) / 100;
        marker_x = std::clamp(marker_x, rect.GetLeft(), rect.GetRight());
        dc.SetPen(wxPen(wxColour(255, 255, 255), 3));
        dc.DrawLine(marker_x, rect.GetTop(), marker_x, rect.GetBottom());
        dc.SetPen(wxPen(wxColour(33, 33, 33), 1));
        dc.DrawLine(marker_x, rect.GetTop(), marker_x, rect.GetBottom());
    }

    void on_left_down(wxMouseEvent &evt)
    {
        if (m_multi_mode)
            return;
        if (!HasCapture())
            CaptureMouse();
        m_dragging = true;
        update_from_x(evt.GetX(), false);
    }

    void on_left_up(wxMouseEvent &evt)
    {
        if (m_multi_mode) {
            wxCommandEvent click_evt(wxEVT_BUTTON, GetId());
            click_evt.SetEventObject(this);
            ProcessWindowEvent(click_evt);
            return;
        }
        if (m_dragging)
            update_from_x(evt.GetX(), true);
        m_dragging = false;
        if (HasCapture())
            ReleaseMouse();
    }

    void on_mouse_move(wxMouseEvent &evt)
    {
        if (m_dragging && evt.LeftIsDown())
            update_from_x(evt.GetX(), false);
    }

    void on_capture_lost(wxMouseCaptureLostEvent &)
    {
        m_dragging = false;
    }

private:
    wxColour m_left;
    wxColour m_right;
    bool     m_multi_mode { false };
    std::vector<wxColour> m_multi_colors;
    std::vector<int>      m_multi_weights;
    int      m_value {50};
    bool     m_dragging {false};
};

class MixedGradientWeightsDialog : public wxDialog
{
public:
    MixedGradientWeightsDialog(wxWindow *parent,
                               const std::vector<unsigned int> &filament_ids,
                               const std::vector<wxColour> &palette,
                               const std::vector<int> &initial_weights)
        : wxDialog(parent, wxID_ANY, _L("Gradient Mix Weights"), wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        m_colors.reserve(filament_ids.size());
        m_weights = normalize_color_match_weights(initial_weights, filament_ids.size());
        for (const unsigned int filament_id : filament_ids) {
            if (filament_id >= 1 && filament_id <= palette.size())
                m_colors.emplace_back(palette[filament_id - 1]);
            else
                m_colors.emplace_back(wxColour("#26A69A"));
        }
        if (m_colors.empty())
            m_colors.emplace_back(wxColour("#26A69A"));

        auto *root = new wxBoxSizer(wxVERTICAL);
        auto *hint = new wxStaticText(this, wxID_ANY, _L("Pick a point in the gradient map to control multi-filament mix."));
        root->Add(hint, 0, wxEXPAND | wxALL, FromDIP(10));

        m_color_map = new MixedFilamentColorMapPanel(this, filament_ids, palette, initial_weights,
                                                     wxSize(FromDIP(240), FromDIP(240)));
        root->Add(m_color_map, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

        for (size_t i = 0; i < filament_ids.size(); ++i) {
            auto *row = new wxBoxSizer(wxHORIZONTAL);
            wxPanel *chip = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(18), FromDIP(18)), wxBORDER_SIMPLE);
            chip->SetBackgroundColour(m_colors[i]);
            row->Add(chip, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
            row->Add(new wxStaticText(this, wxID_ANY, wxString::Format("F%d", int(filament_ids[i]))),
                     0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
            auto *label = new wxStaticText(this, wxID_ANY, wxString::Format("%d%%", m_weights[i]));
            label->SetFont(Label::Body_12);
            row->Add(label, 0, wxALIGN_CENTER_VERTICAL);
            root->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
            m_weight_labels.emplace_back(label);
        }

        root->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, FromDIP(8));
        SetSizerAndFit(root);
        SetMinSize(wxSize(FromDIP(380), std::max(GetSize().GetHeight(), FromDIP(460))));
        update_weight_labels();

        if (m_color_map) {
            m_color_map->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
                m_weights = m_color_map ? m_color_map->normalized_weights() : m_weights;
                update_weight_labels();
            });
        }
    }

    std::vector<int> normalized_weights() const
    {
        return m_color_map ? m_color_map->normalized_weights() : m_weights;
    }

private:
    void update_weight_labels()
    {
        for (size_t i = 0; i < m_weight_labels.size() && i < m_weights.size(); ++i) {
            if (m_weight_labels[i])
                m_weight_labels[i]->SetLabel(wxString::Format("%d%%", m_weights[i]));
        }
        Layout();
    }

private:
    MixedFilamentColorMapPanel *m_color_map { nullptr };
    std::vector<wxColour>       m_colors;
    std::vector<int>            m_weights;
    std::vector<wxStaticText*>  m_weight_labels;
};

// Forward declaration for MixedMixPreview (defined below)
class MixedMixPreview;

struct MixedFilamentPreviewSettings
{
    double nominal_layer_height { 0.2 };
    double mixed_lower_bound { 0.04 };
    double mixed_upper_bound { 0.16 };
    double preferred_a_height { 0.0 };
    double preferred_b_height { 0.0 };
    bool   local_z_mode { false };
    size_t wall_loops { 1 };
};

// Inline editor panel for configuring a single mixed filament
class MixedFilamentConfigPanel : public wxPanel
{
public:
    using OnChangeFn = std::function<void(const MixedFilament &)>;

    MixedFilamentConfigPanel(wxWindow *parent,
                             size_t mixed_id,
                             const MixedFilament &mf,
                             size_t num_physical,
                             const std::vector<std::string> &physical_colors,
                             const std::vector<double> &nozzle_diameters,
                             const std::vector<wxColour> &palette,
                             const MixedFilamentPreviewSettings &preview_settings,
                             bool bias_mode_enabled,
                             OnChangeFn on_change = {});

    // Get the updated mixed filament data
    MixedFilament get_mixed_filament() const { return m_mf; }
    bool has_changes() const { return m_has_changes; }
    static int effective_local_z_preview_mix_b_percent(const MixedFilament &mf,
                                                       const MixedFilamentPreviewSettings &preview_settings);

private:
    void build_ui();
    void update_preview();
    void update_local_z_breakdown();
    void update_component_picker_visuals();

    size_t                          m_mixed_id;
    MixedFilament                   m_mf;
    size_t                          m_num_physical;
    std::vector<std::string>        m_physical_colors;
    std::vector<double>             m_nozzle_diameters;
    std::vector<wxColour>           m_palette;
    MixedFilamentPreviewSettings    m_preview_settings;
    bool                            m_bias_mode_enabled = false;
    bool                            m_has_changes = false;

    wxChoice                       *m_choice_a = nullptr;
    wxChoice                       *m_choice_b = nullptr;
    wxChoice                       *m_choice_c = nullptr;
    wxChoice                       *m_choice_d = nullptr;
    wxPanel                        *m_picker_a_container = nullptr;
    wxPanel                        *m_picker_b_container = nullptr;
    wxPanel                        *m_picker_c_container = nullptr;
    wxPanel                        *m_picker_d_container = nullptr;
    wxPanel                        *m_picker_a_swatch = nullptr;
    wxPanel                        *m_picker_b_swatch = nullptr;
    wxPanel                        *m_picker_c_swatch = nullptr;
    wxPanel                        *m_picker_d_swatch = nullptr;
    wxStaticText                   *m_picker_a_label = nullptr;
    wxStaticText                   *m_picker_b_label = nullptr;
    wxStaticText                   *m_picker_c_label = nullptr;
    wxStaticText                   *m_picker_d_label = nullptr;
    wxPanel                        *m_surface_offset_target_container = nullptr;
    wxPanel                        *m_surface_offset_target_swatch = nullptr;
    wxStaticText                   *m_surface_offset_target_label = nullptr;
    MixedGradientSelector          *m_blend_selector = nullptr;
    wxStaticText                   *m_blend_label = nullptr;
    wxTextCtrl                     *m_pattern_ctrl = nullptr;
    wxCheckBox                     *m_local_z_limit_checkbox = nullptr;
    wxSpinCtrl                     *m_local_z_limit_spin = nullptr;
    wxSpinCtrlDouble               *m_surface_offset_spin = nullptr;
    std::vector<wxButton*>          m_pattern_quick_buttons;
    MixedMixPreview                *m_mix_preview = nullptr;
    wxStaticText                   *m_breakdown_label = nullptr;
    wxPanel                        *m_swatch = nullptr;
    std::shared_ptr<std::vector<int>> m_selected_weight_state;
    OnChangeFn                       m_on_change;

    // Helper functions (copied from update_mixed_filament_panel)
    static std::vector<unsigned int> decode_gradient_ids(const std::string &s);
    static std::string encode_gradient_ids(const std::vector<unsigned int> &ids);
    static std::vector<unsigned int> decode_manual_pattern_ids(const std::string &pattern,
                                                               unsigned int       a,
                                                               unsigned int       b,
                                                               size_t             num_physical,
                                                               size_t             wall_loops = 0);
    static std::vector<int> decode_gradient_weights(const std::string &s, size_t n);
    static std::vector<int> normalize_gradient_weights(const std::vector<int> &w, size_t n);
    static std::string encode_gradient_weights(const std::vector<int> &w);
    static std::vector<unsigned int> build_weighted_pair_sequence(unsigned int a, unsigned int b, int percent_b, bool limit_cycle = false);
    static std::vector<unsigned int> build_weighted_multi_sequence(const std::vector<unsigned int> &ids,
                                                                   const std::vector<int> &weights,
                                                                   size_t max_cycle_limit = 0);
    static std::string summarize_sequence(const std::vector<unsigned int> &seq);
    static std::string summarize_local_z_breakdown(const MixedFilament &mf,
                                                   const std::vector<int> &weights,
                                                   const MixedFilamentPreviewSettings &preview_settings);
    static std::string blend_from_sequence(const std::vector<std::string> &colors, const std::vector<unsigned int> &seq, const std::string &fallback);
    static std::vector<double> build_local_z_preview_pass_heights(double nominal_layer_height,
                                                                  double lower_bound,
                                                                  double upper_bound,
                                                                  double preferred_a_height,
                                                                  double preferred_b_height,
                                                                  int mix_b_percent,
                                                                  int max_sublayers_limit);
};

class MixedMixPreview : public wxPanel
{
public:
    explicit MixedMixPreview(wxWindow *parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(FromDIP(120), FromDIP(20)));
        Bind(wxEVT_PAINT, &MixedMixPreview::on_paint, this);
    }

    void set_data(const std::vector<wxColour> &palette,
                  const std::vector<unsigned int> &sequence,
                  bool same_layer_mode,
                  const std::vector<double> &surface_offsets_mm,
                  const wxColour &fallback,
                  const wxString &left_overlay,
                  const wxString &right_overlay)
    {
        m_palette    = palette;
        m_sequence   = sequence;
        m_same_layer = same_layer_mode;
        m_surface_offsets_mm = surface_offsets_mm;
        m_fallback   = fallback;
        m_left_overlay = left_overlay;
        m_right_overlay = right_overlay;
        Refresh();
    }

private:
    wxRect preview_rect() const
    {
        const int margin_x = FromDIP(1);
        const int margin_y = FromDIP(1);
        const wxSize sz = GetClientSize();
        return wxRect(margin_x, margin_y, std::max(1, sz.GetWidth() - margin_x * 2), std::max(1, sz.GetHeight() - margin_y * 2));
    }

    wxColour color_for_extruder(unsigned int extruder_id) const
    {
        if (extruder_id >= 1 && extruder_id <= m_palette.size())
            return m_palette[extruder_id - 1];
        return m_fallback;
    }

    double max_active_surface_offset_mm() const
    {
        double max_offset = 0.0;
        for (double offset_mm : m_surface_offsets_mm)
            max_offset = std::max(max_offset, std::abs(offset_mm));
        return std::max(0.001, max_offset);
    }

    int slot_inset_for_extruder(unsigned int extruder_id, int slot_extent) const
    {
        if (extruder_id == 0 || extruder_id >= m_surface_offsets_mm.size() || slot_extent <= 2)
            return 0;

        const double offset_mm = m_surface_offsets_mm[extruder_id];
        if (std::abs(offset_mm) <= EPSILON)
            return 0;

        const double normalized = std::clamp(std::abs(offset_mm) / max_active_surface_offset_mm(), 0.0, 1.0);
        const int inset = int(std::round(normalized * slot_extent * 0.45)) * (offset_mm < 0.0 ? -1 : 1);
        return std::clamp(inset, -std::max(0, slot_extent / 2), std::max(0, slot_extent / 2));
    }

    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();

        const wxRect rect = preview_rect();
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(m_fallback));
        dc.DrawRectangle(rect);

        if (!m_sequence.empty()) {
            if (m_same_layer) {
                // Same-layer preview: full-height stripe lines.
                const int stripes = 24;
                const int stripe_w = std::max(1, rect.GetWidth() / stripes);
                const size_t seq_len = m_sequence.size();
                for (int s = 0; s < stripes; ++s) {
                    const size_t idx = size_t(s % int(seq_len));
                    const unsigned int extruder_id = m_sequence[idx];
                    dc.SetBrush(wxBrush(color_for_extruder(m_sequence[idx])));
                    const int x = rect.GetLeft() + s * stripe_w;
                    const int w = (s == stripes - 1) ? (rect.GetRight() - x + 1) : stripe_w;
                    const int inset = slot_inset_for_extruder(extruder_id, w);
                    wxRect draw_rect(x + inset / 2, rect.GetTop(), std::max(1, w - inset), rect.GetHeight());
                    draw_rect.Intersect(rect);
                    if (draw_rect.GetWidth() > 0)
                        dc.DrawRectangle(draw_rect);
                }
            } else {
                const int bars = 24;
                const int bar_w = std::max(1, rect.GetWidth() / bars);
                for (int i = 0; i < bars; ++i) {
                    size_t idx = 0;
                    if (m_sequence.size() > size_t(bars))
                        idx = (size_t(i) * m_sequence.size()) / size_t(bars);
                    else
                        idx = size_t(i) % m_sequence.size();
                    const unsigned int extruder_id = m_sequence[idx];
                    dc.SetBrush(wxBrush(color_for_extruder(extruder_id)));
                    const int x = rect.GetLeft() + i * bar_w;
                    const int w = (i == bars - 1) ? (rect.GetRight() - x + 1) : bar_w;
                    const int inset = slot_inset_for_extruder(extruder_id, w);
                    wxRect draw_rect(x + inset / 2, rect.GetTop(), std::max(1, w - inset), rect.GetHeight());
                    draw_rect.Intersect(rect);
                    if (draw_rect.GetWidth() > 0)
                        dc.DrawRectangle(draw_rect);
                }
            }
        }

        auto draw_outlined_text = [this, &dc](const wxString &text, int x, int y) {
            if (text.empty())
                return;
            dc.SetTextForeground(wxColour(255, 255, 255));
            const int outline_radius = std::max(2, FromDIP(2));
            for (int ox = -outline_radius; ox <= outline_radius; ++ox) {
                for (int oy = -outline_radius; oy <= outline_radius; ++oy) {
                    if (ox == 0 && oy == 0)
                        continue;
                    dc.DrawText(text, x + ox, y + oy);
                }
            }
            dc.SetTextForeground(wxColour(22, 22, 22));
            dc.DrawText(text, x, y);
        };

        wxCoord left_w = 0, left_h = 0;
        wxCoord right_w = 0, right_h = 0;
        dc.GetTextExtent(m_left_overlay, &left_w, &left_h);
        dc.GetTextExtent(m_right_overlay, &right_w, &right_h);
        const int text_y = rect.GetTop() + std::max(0, (rect.GetHeight() - int(std::max(left_h, right_h))) / 2);
        const int pad = FromDIP(6);
        if (!m_left_overlay.empty())
            draw_outlined_text(m_left_overlay, rect.GetLeft() + pad, text_y);
        if (!m_right_overlay.empty())
            draw_outlined_text(m_right_overlay, rect.GetRight() - pad - int(right_w), text_y);

        const bool is_dark = wxGetApp().dark_mode();
        dc.SetPen(wxPen(is_dark ? wxColour(110, 110, 110) : wxColour(170, 170, 170), 1));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(rect);
    }

private:
    std::vector<wxColour>       m_palette;
    std::vector<unsigned int>   m_sequence;
    std::vector<double>         m_surface_offsets_mm;
    bool                        m_same_layer { false };
    wxColour                    m_fallback { wxColour(38, 166, 154) };
    wxString                    m_left_overlay;
    wxString                    m_right_overlay;
};

// Implementation of MixedFilamentConfigPanel helper functions
static std::vector<std::string> split_manual_pattern_preview_groups(const std::string &pattern)
{
    std::vector<std::string> groups;
    if (pattern.empty())
        return groups;

    std::string current;
    for (const char c : pattern) {
        if (c == ',') {
            if (!current.empty()) {
                groups.emplace_back(std::move(current));
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty())
        groups.emplace_back(std::move(current));
    return groups;
}

static unsigned int decode_manual_pattern_preview_token(char token, unsigned int component_a, unsigned int component_b, size_t num_physical)
{
    unsigned int extruder_id = 0;
    if (token == '1')
        extruder_id = component_a;
    else if (token == '2')
        extruder_id = component_b;
    else if (token >= '3' && token <= '9')
        extruder_id = unsigned(token - '0');

    return (extruder_id >= 1 && extruder_id <= num_physical) ? extruder_id : 0;
}

static std::vector<unsigned int> build_grouped_manual_pattern_preview_sequence(const std::string &pattern,
                                                                               unsigned int       component_a,
                                                                               unsigned int       component_b,
                                                                               size_t             num_physical,
                                                                               size_t             wall_loops)
{
    std::vector<unsigned int> sequence;
    if (num_physical == 0)
        return sequence;

    const std::string normalized = MixedFilamentManager::normalize_manual_pattern(pattern);
    if (normalized.empty())
        return sequence;

    const std::vector<std::string> groups = split_manual_pattern_preview_groups(normalized);
    if (groups.empty())
        return sequence;

    if (groups.size() == 1) {
        sequence.reserve(normalized.size());
        for (const char token : normalized) {
            const unsigned int extruder_id =
                decode_manual_pattern_preview_token(token, component_a, component_b, num_physical);
            if (extruder_id != 0)
                sequence.emplace_back(extruder_id);
        }
        return sequence;
    }

    constexpr size_t k_max_preview_cycle = 48;
    size_t cycle = 1;
    for (const std::string &group : groups) {
        if (group.empty())
            continue;
        cycle = std::lcm(cycle, group.size());
        if (cycle >= k_max_preview_cycle) {
            cycle = k_max_preview_cycle;
            break;
        }
    }

    const size_t preview_wall_loops = std::max<size_t>(1, wall_loops == 0 ? groups.size() : wall_loops);
    sequence.reserve(preview_wall_loops * cycle);
    for (size_t layer_idx = 0; layer_idx < cycle; ++layer_idx) {
        for (size_t wall_idx = 0; wall_idx < preview_wall_loops; ++wall_idx) {
            const std::string &group = groups[std::min(wall_idx, groups.size() - 1)];
            if (group.empty())
                continue;
            const char token = group[layer_idx % group.size()];
            const unsigned int extruder_id =
                decode_manual_pattern_preview_token(token, component_a, component_b, num_physical);
            if (extruder_id != 0)
                sequence.emplace_back(extruder_id);
        }
    }

    return sequence;
}

std::vector<unsigned int> MixedFilamentConfigPanel::decode_gradient_ids(const std::string &s)
{
    std::vector<unsigned int> ids;
    if (s.empty())
        return ids;

    bool seen[10] = { false };
    for (const char c : s) {
        if (c < '1' || c > '9')
            continue;
        const unsigned int id = unsigned(c - '0');
        if (seen[id])
            continue;
        seen[id] = true;
        ids.emplace_back(id);
    }
    return ids;
}

std::string MixedFilamentConfigPanel::encode_gradient_ids(const std::vector<unsigned int> &ids)
{
    std::string out;
    bool seen[10] = { false };
    for (const unsigned int id : ids) {
        if (id == 0 || id > 9 || seen[id])
            continue;
        seen[id] = true;
        out.push_back(char('0' + id));
    }
    return out;
}

std::vector<unsigned int> MixedFilamentConfigPanel::decode_manual_pattern_ids(const std::string &pattern,
                                                                              unsigned int       a,
                                                                              unsigned int       b,
                                                                              size_t             num_physical,
                                                                              size_t             wall_loops)
{
    return build_grouped_manual_pattern_preview_sequence(pattern, a, b, num_physical, wall_loops);
}

std::vector<int> MixedFilamentConfigPanel::decode_gradient_weights(const std::string &s, size_t n)
{
    std::vector<int> w;
    if (s.empty() || n == 0)
        return w;

    std::string token;
    for (const char c : s) {
        if (c >= '0' && c <= '9') {
            token.push_back(c);
            continue;
        }
        if (!token.empty()) {
            w.emplace_back(std::max(0, std::atoi(token.c_str())));
            token.clear();
        }
    }
    if (!token.empty())
        w.emplace_back(std::max(0, std::atoi(token.c_str())));
    if (w.size() != n)
        w.clear();
    return w;
}

std::vector<int> MixedFilamentConfigPanel::normalize_gradient_weights(const std::vector<int> &w, size_t n)
{
    std::vector<int> out = w;
    if (out.size() != n) out.assign(n, n > 0 ? int(100 / n) : 0);
    int sum = 0;
    for (int &v : out) { v = std::max(0, v); sum += v; }
    if (sum <= 0 && n > 0) { out.assign(n, 0); out[0] = 100; return out; }
    std::vector<double> rem(n, 0.);
    int assigned = 0;
    for (size_t i = 0; i < n; ++i) {
        const double exact = 100.0 * double(out[i]) / double(sum);
        out[i] = int(std::floor(exact));
        rem[i] = exact - double(out[i]);
        assigned += out[i];
    }
    int missing = std::max(0, 100 - assigned);
    while (missing > 0) {
        size_t best = 0;
        double best_rem = -1.0;
        for (size_t i = 0; i < rem.size(); ++i) {
            if (rem[i] > best_rem) { best_rem = rem[i]; best = i; }
        }
        ++out[best];
        rem[best] = 0.0;
        --missing;
    }
    return out;
}

std::string MixedFilamentConfigPanel::encode_gradient_weights(const std::vector<int> &w)
{
    std::ostringstream out;
    for (size_t i = 0; i < w.size(); ++i) {
        if (i > 0)
            out << '/';
        out << std::max(0, w[i]);
    }
    return out.str();
}

namespace {

std::pair<int, int> effective_pair_preview_ratios(int percent_b)
{
    const int mix_b = std::clamp(percent_b, 0, 100);
    int       ratio_a = 1;
    int       ratio_b = 0;

    if (mix_b >= 100) {
        ratio_a = 0;
        ratio_b = 1;
    } else if (mix_b > 0) {
        const int pct_b      = mix_b;
        const int pct_a      = 100 - pct_b;
        const bool b_is_major = pct_b >= pct_a;
        const int major_pct  = b_is_major ? pct_b : pct_a;
        const int minor_pct  = b_is_major ? pct_a : pct_b;
        const int major_layers =
            std::max(1, int(std::lround(double(major_pct) / double(std::max(1, minor_pct)))));
        ratio_a = b_is_major ? 1 : major_layers;
        ratio_b = b_is_major ? major_layers : 1;
    }

    if (ratio_a > 0 && ratio_b > 0) {
        const int g = std::gcd(ratio_a, ratio_b);
        if (g > 1) {
            ratio_a /= g;
            ratio_b /= g;
        }
    }

    return { std::max(0, ratio_a), std::max(0, ratio_b) };
}

std::vector<unsigned int> build_effective_pair_preview_sequence(unsigned int component_a,
                                                                unsigned int component_b,
                                                                int          percent_b,
                                                                bool         limit_cycle)
{
    std::vector<unsigned int> sequence;
    if (component_a == 0 || component_b == 0 || component_a == component_b)
        return sequence;

    auto [ratio_a, ratio_b] = effective_pair_preview_ratios(percent_b);
    constexpr int k_max_cycle = 24;
    if (limit_cycle && ratio_a > 0 && ratio_b > 0 && ratio_a + ratio_b > k_max_cycle) {
        const double scale = double(k_max_cycle) / double(ratio_a + ratio_b);
        ratio_a = std::max(1, int(std::round(double(ratio_a) * scale)));
        ratio_b = std::max(1, int(std::round(double(ratio_b) * scale)));
    }
    if (ratio_a == 0 && ratio_b == 0)
        ratio_a = 1;

    const int cycle = std::max(1, ratio_a + ratio_b);
    sequence.reserve(size_t(cycle));
    for (int pos = 0; pos < cycle; ++pos) {
        const int b_before = (pos * ratio_b) / cycle;
        const int b_after  = ((pos + 1) * ratio_b) / cycle;
        sequence.emplace_back((b_after > b_before) ? component_b : component_a);
    }
    return sequence;
}

std::string format_preview_sequence_percent(int count, int total)
{
    if (count <= 0 || total <= 0)
        return "";

    const double percent         = 100.0 * double(count) / double(total);
    const double rounded_tenths  = std::round(percent * 10.0) / 10.0;
    const double nearest_integer = std::round(rounded_tenths);
    if (std::abs(rounded_tenths - nearest_integer) < 1e-6)
        return wxString::Format("%d%%", int(nearest_integer)).ToStdString();
    return wxString::Format("%.1f%%", rounded_tenths).ToStdString();
}

} // namespace

std::vector<unsigned int> MixedFilamentConfigPanel::build_weighted_pair_sequence(unsigned int a,
                                                                                 unsigned int b,
                                                                                 int          percent_b,
                                                                                 bool         limit_cycle)
{
    return build_effective_pair_preview_sequence(a, b, percent_b, limit_cycle);
}

static void reduce_weight_counts_to_cycle_limit(std::vector<int> &counts, size_t cycle_limit)
{
    if (counts.empty() || cycle_limit == 0)
        return;

    int total = std::accumulate(counts.begin(), counts.end(), 0);
    if (total <= 0 || size_t(total) <= cycle_limit)
        return;

    std::vector<size_t> positive_indices;
    positive_indices.reserve(counts.size());
    for (size_t i = 0; i < counts.size(); ++i)
        if (counts[i] > 0)
            positive_indices.emplace_back(i);

    if (positive_indices.empty()) {
        counts.assign(counts.size(), 0);
        return;
    }

    std::vector<int> reduced(counts.size(), 0);
    if (cycle_limit < positive_indices.size()) {
        std::sort(positive_indices.begin(), positive_indices.end(), [&counts](size_t lhs, size_t rhs) {
            if (counts[lhs] != counts[rhs])
                return counts[lhs] > counts[rhs];
            return lhs < rhs;
        });
        for (size_t i = 0; i < cycle_limit; ++i)
            reduced[positive_indices[i]] = 1;
        counts = std::move(reduced);
        return;
    }

    size_t remaining_slots = cycle_limit;
    for (const size_t idx : positive_indices) {
        reduced[idx] = 1;
        --remaining_slots;
    }

    int total_extras = 0;
    std::vector<int> extra_counts(counts.size(), 0);
    for (const size_t idx : positive_indices) {
        extra_counts[idx] = std::max(0, counts[idx] - 1);
        total_extras += extra_counts[idx];
    }
    if (remaining_slots == 0 || total_extras <= 0) {
        counts = std::move(reduced);
        return;
    }

    std::vector<double> remainders(counts.size(), -1.0);
    size_t assigned_slots = 0;
    for (const size_t idx : positive_indices) {
        if (extra_counts[idx] == 0)
            continue;
        const double exact = double(remaining_slots) * double(extra_counts[idx]) / double(total_extras);
        const int assigned = int(std::floor(exact));
        reduced[idx] += assigned;
        assigned_slots += size_t(assigned);
        remainders[idx] = exact - double(assigned);
    }

    size_t missing_slots = remaining_slots > assigned_slots ? (remaining_slots - assigned_slots) : size_t(0);
    while (missing_slots > 0) {
        size_t best_idx = size_t(-1);
        double best_remainder = -1.0;
        int    best_extra = -1;
        for (const size_t idx : positive_indices) {
            if (extra_counts[idx] == 0)
                continue;
            if (remainders[idx] > best_remainder ||
                (std::abs(remainders[idx] - best_remainder) <= 1e-9 && extra_counts[idx] > best_extra) ||
                (std::abs(remainders[idx] - best_remainder) <= 1e-9 && extra_counts[idx] == best_extra && idx < best_idx)) {
                best_idx = idx;
                best_remainder = remainders[idx];
                best_extra = extra_counts[idx];
            }
        }
        if (best_idx == size_t(-1))
            break;
        ++reduced[best_idx];
        remainders[best_idx] = -1.0;
        --missing_slots;
    }

    counts = std::move(reduced);
}

std::vector<unsigned int> MixedFilamentConfigPanel::build_weighted_multi_sequence(const std::vector<unsigned int> &ids,
                                                                                  const std::vector<int> &weights,
                                                                                  size_t max_cycle_limit)
{
    std::vector<unsigned int> seq;
    if (ids.empty())
        return seq;

    std::vector<unsigned int> filtered_ids;
    std::vector<int> counts;
    filtered_ids.reserve(ids.size());
    counts.reserve(ids.size());

    std::vector<int> normalized = normalize_gradient_weights(weights, ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        const int weight = (i < normalized.size()) ? std::max(0, normalized[i]) : 0;
        if (weight <= 0)
            continue;
        filtered_ids.emplace_back(ids[i]);
        counts.emplace_back(weight);
    }
    if (filtered_ids.empty()) {
        filtered_ids = ids;
        counts.assign(ids.size(), 1);
    }

    int g = 0;
    for (const int c : counts)
        g = std::gcd(g, std::max(1, c));
    if (g > 1) {
        for (int &c : counts)
            c = std::max(1, c / g);
    }

    constexpr size_t k_max_cycle = 48;
    const size_t effective_cycle_limit =
        max_cycle_limit > 0 ? std::min(k_max_cycle, std::max<size_t>(1, max_cycle_limit)) : k_max_cycle;
    reduce_weight_counts_to_cycle_limit(counts, effective_cycle_limit);

    std::vector<unsigned int> reduced_ids;
    std::vector<int> reduced_counts;
    reduced_ids.reserve(filtered_ids.size());
    reduced_counts.reserve(counts.size());
    for (size_t i = 0; i < counts.size(); ++i) {
        if (counts[i] <= 0)
            continue;
        reduced_ids.emplace_back(filtered_ids[i]);
        reduced_counts.emplace_back(counts[i]);
    }
    if (reduced_ids.empty())
        return seq;
    filtered_ids = std::move(reduced_ids);
    counts = std::move(reduced_counts);

    const int total = std::accumulate(counts.begin(), counts.end(), 0);
    if (total <= 0)
        return seq;

    const size_t cycle = size_t(total);

    seq.reserve(cycle);
    std::vector<int> emitted(counts.size(), 0);
    for (size_t pos = 0; pos < cycle; ++pos) {
        size_t best_idx = 0;
        double best_score = -1e9;
        for (size_t i = 0; i < counts.size(); ++i) {
            const double target = double(pos + 1) * double(counts[i]) / double(total);
            const double score = target - double(emitted[i]);
            if (score > best_score) {
                best_score = score;
                best_idx = i;
            }
        }
        ++emitted[best_idx];
        seq.emplace_back(filtered_ids[best_idx]);
    }
    if (seq.empty())
        seq = filtered_ids;
    return seq;
}


std::vector<double> MixedFilamentConfigPanel::build_local_z_preview_pass_heights(double nominal_layer_height,
                                                                                 double lower_bound,
                                                                                 double upper_bound,
                                                                                 double preferred_a_height,
                                                                                 double preferred_b_height,
                                                                                 int mix_b_percent,
                                                                                 int max_sublayers_limit)
{
    if (nominal_layer_height <= EPSILON)
        return {};

    const double base_height = nominal_layer_height;
    const double lo = std::max<double>(0.01, lower_bound);
    const double hi = std::max<double>(lo, upper_bound);
    const size_t max_passes_limit = max_sublayers_limit >= 2 ? size_t(max_sublayers_limit) : size_t(0);

    auto fit_pass_heights_to_interval = [](std::vector<double> &passes, double total_height, double local_lo, double local_hi) {
        if (passes.empty() || total_height <= EPSILON)
            return false;

        const auto within = [local_lo, local_hi](double value) {
            return value >= local_lo - 1e-6 && value <= local_hi + 1e-6;
        };

        double sum = 0.0;
        for (const double h : passes)
            sum += h;

        double delta = total_height - sum;
        if (std::abs(delta) > 1e-6) {
            if (delta > 0.0) {
                for (double &h : passes) {
                    if (delta <= 1e-6)
                        break;
                    const double room = local_hi - h;
                    if (room <= 1e-6)
                        continue;
                    const double take = std::min(room, delta);
                    h += take;
                    delta -= take;
                }
            } else {
                for (auto it = passes.rbegin(); it != passes.rend() && delta < -1e-6; ++it) {
                    const double room = *it - local_lo;
                    if (room <= 1e-6)
                        continue;
                    const double take = std::min(room, -delta);
                    *it -= take;
                    delta += take;
                }
            }
        }

        if (std::abs(delta) > 1e-6)
            return false;
        return std::all_of(passes.begin(), passes.end(), within);
    };

    auto build_uniform = [&fit_pass_heights_to_interval, base_height, lo, hi, max_passes_limit]() {
        std::vector<double> out;
        size_t min_passes = size_t(std::max<double>(1.0, std::ceil((base_height - EPSILON) / hi)));
        size_t max_passes = size_t(std::max<double>(1.0, std::floor((base_height + EPSILON) / lo)));
        size_t pass_count = min_passes;

        if (max_passes >= min_passes) {
            const double target_step = 0.5 * (lo + hi);
            const size_t target_passes =
                size_t(std::max<double>(1.0, std::llround(base_height / std::max<double>(target_step, EPSILON))));
            pass_count = std::clamp(target_passes, min_passes, max_passes);
        }

        if (max_passes_limit > 0 && pass_count > max_passes_limit)
            pass_count = max_passes_limit;

        if (pass_count == 1 && base_height >= 2.0 * lo - EPSILON && max_passes >= 2)
            pass_count = 2;

        if (pass_count <= 1) {
            out.emplace_back(base_height);
            return out;
        }

        out.assign(pass_count, base_height / double(pass_count));
        double accumulated = 0.0;
        for (size_t i = 0; i + 1 < out.size(); ++i)
            accumulated += out[i];
        out.back() = std::max<double>(EPSILON, base_height - accumulated);
        if (!fit_pass_heights_to_interval(out, base_height, lo, hi) && max_passes_limit == 0) {
            out.assign(pass_count, base_height / double(pass_count));
            accumulated = 0.0;
            for (size_t i = 0; i + 1 < out.size(); ++i)
                accumulated += out[i];
            out.back() = std::max<double>(EPSILON, base_height - accumulated);
        }
        return out;
    };

    auto build_alternating = [&build_uniform, &fit_pass_heights_to_interval, base_height, lo, hi, max_passes_limit](double gradient_h_a, double gradient_h_b) {
        if (base_height < 2.0 * lo - EPSILON)
            return std::vector<double>{ base_height };

        const double cycle_h = std::max<double>(EPSILON, gradient_h_a + gradient_h_b);
        const double ratio_a = std::clamp(gradient_h_a / cycle_h, 0.0, 1.0);

        size_t min_passes = size_t(std::max<double>(2.0, std::ceil((base_height - EPSILON) / hi)));
        if ((min_passes % 2) != 0)
            ++min_passes;

        size_t max_passes = size_t(std::max<double>(2.0, std::floor((base_height + EPSILON) / lo)));
        if ((max_passes % 2) != 0)
            --max_passes;
        if (max_passes_limit > 0) {
            size_t capped_limit = std::max<size_t>(2, max_passes_limit);
            if ((capped_limit % 2) != 0)
                --capped_limit;
            if (capped_limit >= 2)
                max_passes = std::min(max_passes, capped_limit);
        }
        if (max_passes < 2)
            return build_uniform();
        if (min_passes > max_passes)
            min_passes = max_passes;
        if (min_passes < 2)
            min_passes = 2;
        if ((min_passes % 2) != 0)
            ++min_passes;
        if (min_passes > max_passes)
            return build_uniform();

        const double target_step = 0.5 * (lo + hi);
        size_t target_passes =
            size_t(std::max<double>(2.0, std::llround(base_height / std::max<double>(target_step, EPSILON))));
        if ((target_passes % 2) != 0) {
            const size_t round_up = (target_passes < max_passes) ? (target_passes + 1) : max_passes;
            const size_t round_down = (target_passes > min_passes) ? (target_passes - 1) : min_passes;
            if (round_up > max_passes)
                target_passes = round_down;
            else if (round_down < min_passes)
                target_passes = round_up;
            else
                target_passes = ((round_up - target_passes) <= (target_passes - round_down)) ? round_up : round_down;
        }
        target_passes = std::clamp(target_passes, min_passes, max_passes);

        bool                has_best           = false;
        std::vector<double> best_passes;
        double              best_ratio_error   = 0.0;
        size_t              best_pass_distance = 0;
        double              best_max_height    = 0.0;
        size_t              best_pass_count    = 0;

        for (size_t pass_count = min_passes; pass_count <= max_passes; pass_count += 2) {
            const size_t pair_count = pass_count / 2;
            if (pair_count == 0)
                continue;
            const double pair_h = base_height / double(pair_count);

            const double h_a_min = std::max(lo, pair_h - hi);
            const double h_a_max = std::min(hi, pair_h - lo);
            if (h_a_min > h_a_max + EPSILON)
                continue;

            const double h_a = std::clamp(pair_h * ratio_a, h_a_min, h_a_max);
            const double h_b = pair_h - h_a;

            std::vector<double> out;
            out.reserve(pass_count);
            for (size_t pair_idx = 0; pair_idx < pair_count; ++pair_idx) {
                out.emplace_back(h_a);
                out.emplace_back(h_b);
            }
            if (!fit_pass_heights_to_interval(out, base_height, lo, hi))
                continue;

            const double ratio_actual = (h_a + h_b > EPSILON) ? (h_a / (h_a + h_b)) : 0.5;
            const double ratio_error  = std::abs(ratio_actual - ratio_a);
            const size_t pass_distance =
                (pass_count > target_passes) ? (pass_count - target_passes) : (target_passes - pass_count);
            const double max_height = std::max(h_a, h_b);

            const bool better_ratio         = !has_best || (ratio_error + 1e-6 < best_ratio_error);
            const bool similar_ratio        = has_best && std::abs(ratio_error - best_ratio_error) <= 1e-6;
            const bool better_distance      = similar_ratio && (pass_distance < best_pass_distance);
            const bool similar_distance     = similar_ratio && (pass_distance == best_pass_distance);
            const bool better_max_height    = similar_distance && (max_height + 1e-6 < best_max_height);
            const bool similar_max_height   = similar_distance && std::abs(max_height - best_max_height) <= 1e-6;
            const bool better_pass_count    = similar_max_height && (pass_count > best_pass_count);

            if (better_ratio || better_distance || better_max_height || better_pass_count) {
                has_best = true;
                best_passes = std::move(out);
                best_ratio_error = ratio_error;
                best_pass_distance = pass_distance;
                best_max_height = max_height;
                best_pass_count = pass_count;
            }
        }

        return has_best ? best_passes : build_uniform();
    };

    if (preferred_a_height > EPSILON || preferred_b_height > EPSILON) {
        std::vector<double> cadence_unit;
        if (preferred_a_height > EPSILON)
            cadence_unit.push_back(std::clamp(preferred_a_height, lo, hi));
        if (preferred_b_height > EPSILON)
            cadence_unit.push_back(std::clamp(preferred_b_height, lo, hi));

        if (!cadence_unit.empty()) {
            std::vector<double> out;
            out.reserve(size_t(std::ceil(base_height / lo)) + 2);

            double z_used = 0.0;
            size_t idx = 0;
            size_t guard = 0;
            while (z_used + cadence_unit[idx] < base_height - EPSILON && guard++ < 100000) {
                out.push_back(cadence_unit[idx]);
                z_used += cadence_unit[idx];
                idx = (idx + 1) % cadence_unit.size();
            }

            const double remainder = base_height - z_used;
            if (remainder > EPSILON)
                out.push_back(remainder);

            if (fit_pass_heights_to_interval(out, base_height, lo, hi) &&
                (max_passes_limit == 0 || out.size() <= max_passes_limit))
                return out;
        }

        if (preferred_a_height > EPSILON && preferred_b_height > EPSILON)
            return build_alternating(preferred_a_height, preferred_b_height);
        return build_uniform();
    }

    const int mix_b = std::clamp(mix_b_percent, 0, 100);
    const double pct_b = double(mix_b) / 100.0;
    const double pct_a = 1.0 - pct_b;
    const double gradient_h_a = lo + pct_a * (hi - lo);
    const double gradient_h_b = lo + pct_b * (hi - lo);
    return build_alternating(gradient_h_a, gradient_h_b);
}

int MixedFilamentConfigPanel::effective_local_z_preview_mix_b_percent(const MixedFilament &mf,
                                                                      const MixedFilamentPreviewSettings &preview_settings)
{
    if (!preview_settings.local_z_mode)
        return std::clamp(mf.mix_b_percent, 0, 100);

    const std::string normalized_pattern = MixedFilamentManager::normalize_manual_pattern(mf.manual_pattern);
    if (!normalized_pattern.empty() || mf.distribution_mode == int(MixedFilament::SameLayerPointillisme))
        return std::clamp(mf.mix_b_percent, 0, 100);

    const std::vector<unsigned int> gradient_ids = decode_gradient_ids(mf.gradient_component_ids);
    if (gradient_ids.size() >= 3)
        return std::clamp(mf.mix_b_percent, 0, 100);

    const std::vector<double> pass_heights = build_local_z_preview_pass_heights(preview_settings.nominal_layer_height,
                                                                                 preview_settings.mixed_lower_bound,
                                                                                 preview_settings.mixed_upper_bound,
                                                                                 preview_settings.preferred_a_height,
                                                                                 preview_settings.preferred_b_height,
                                                                                 mf.mix_b_percent,
                                                                                 0);
    if (pass_heights.empty())
        return std::clamp(mf.mix_b_percent, 0, 100);

    double expected_h_a = preview_settings.preferred_a_height;
    double expected_h_b = preview_settings.preferred_b_height;
    if (expected_h_a <= EPSILON && expected_h_b <= EPSILON) {
        const int mix_b = std::clamp(mf.mix_b_percent, 0, 100);
        const double pct_b = double(mix_b) / 100.0;
        const double pct_a = 1.0 - pct_b;
        const double lo = std::max<double>(0.01, preview_settings.mixed_lower_bound);
        const double hi = std::max<double>(lo, preview_settings.mixed_upper_bound);
        expected_h_a = lo + pct_a * (hi - lo);
        expected_h_b = lo + pct_b * (hi - lo);
    }

    auto choose_start_with_component_a = [](const std::vector<double> &passes, double local_expected_h_a, double local_expected_h_b) {
        double err_ab = 0.0;
        double err_ba = 0.0;
        for (size_t pass_i = 0; pass_i < passes.size(); ++pass_i) {
            const double expected_ab = (pass_i % 2) == 0 ? local_expected_h_a : local_expected_h_b;
            const double expected_ba = (pass_i % 2) == 0 ? local_expected_h_b : local_expected_h_a;
            err_ab += std::abs(passes[pass_i] - expected_ab);
            err_ba += std::abs(passes[pass_i] - expected_ba);
        }
        if (err_ab + 1e-6 < err_ba)
            return true;
        if (err_ba + 1e-6 < err_ab)
            return false;
        return local_expected_h_a >= local_expected_h_b;
    };

    const bool start_with_a = choose_start_with_component_a(pass_heights, expected_h_a, expected_h_b);
    double total_a = 0.0;
    double total_b = 0.0;
    for (size_t pass_i = 0; pass_i < pass_heights.size(); ++pass_i) {
        const bool even_pass = (pass_i % 2) == 0;
        const bool pass_is_a = even_pass ? start_with_a : !start_with_a;
        if (pass_is_a)
            total_a += pass_heights[pass_i];
        else
            total_b += pass_heights[pass_i];
    }

    const double total = total_a + total_b;
    if (total <= EPSILON)
        return std::clamp(mf.mix_b_percent, 0, 100);
    return std::clamp(int(std::lround(100.0 * total_b / total)), 0, 100);
}

static bool mixed_filament_supports_bias_apparent_color(const MixedFilament &mf,
                                                        const MixedFilamentPreviewSettings &preview_settings,
                                                        bool                                bias_mode_enabled)
{
    if (!bias_mode_enabled)
        return false;
    if (preview_settings.local_z_mode)
        return false;
    if (mf.distribution_mode == int(MixedFilament::SameLayerPointillisme))
        return false;
    if (!MixedFilamentManager::normalize_manual_pattern(mf.manual_pattern).empty())
        return false;
    if (mf.gradient_component_ids.size() >= 3)
        return false;
    return mf.component_a >= 1 && mf.component_b >= 1 && mf.component_a != mf.component_b;
}

static double mixed_filament_reference_nozzle_mm(unsigned int               component_a,
                                                 unsigned int               component_b,
                                                 const std::vector<double> &nozzle_diameters)
{
    std::vector<double> samples;
    samples.reserve(2);

    auto append_if_valid = [&samples, &nozzle_diameters](unsigned int component_id) {
        if (component_id >= 1 && component_id <= nozzle_diameters.size())
            samples.emplace_back(std::max(0.05, nozzle_diameters[size_t(component_id - 1)]));
    };

    append_if_valid(component_a);
    append_if_valid(component_b);

    if (samples.empty())
        return 0.4;
    return std::accumulate(samples.begin(), samples.end(), 0.0) / double(samples.size());
}

static double mixed_filament_bias_limit_mm(const MixedFilament &mf, const std::vector<double> &nozzle_diameters)
{
    const double reference_nozzle_mm = mixed_filament_reference_nozzle_mm(mf.component_a, mf.component_b, nozzle_diameters);
    return MixedFilamentManager::max_pair_bias_mm(float(reference_nozzle_mm));
}

static float mixed_filament_single_surface_offset_value(const MixedFilament       &mf,
                                                        const std::vector<double> &nozzle_diameters)
{
    const double reference_nozzle_mm = mixed_filament_reference_nozzle_mm(mf.component_a, mf.component_b, nozzle_diameters);
    return MixedFilamentManager::bias_ui_value_from_surface_offsets(
        mf.component_a_surface_offset,
        mf.component_b_surface_offset,
        float(reference_nozzle_mm));
}

static std::pair<float, float> mixed_filament_single_surface_offset_pair(const MixedFilament       &mf,
                                                                         float                      value,
                                                                         const std::vector<double> &nozzle_diameters)
{
    const double reference_nozzle_mm = mixed_filament_reference_nozzle_mm(mf.component_a, mf.component_b, nozzle_diameters);
    return MixedFilamentManager::surface_offset_pair_from_signed_bias(value, float(reference_nozzle_mm));
}

static std::pair<int, int> mixed_filament_apparent_pair_percentages(const MixedFilament               &mf,
                                                                    const MixedFilamentPreviewSettings &preview_settings,
                                                                    const std::vector<double>          &nozzle_diameters,
                                                                    bool                                bias_mode_enabled)
{
    const int base_b = MixedFilamentConfigPanel::effective_local_z_preview_mix_b_percent(mf, preview_settings);
    if (!mixed_filament_supports_bias_apparent_color(mf, preview_settings, bias_mode_enabled))
        return { 100 - base_b, base_b };

    const double reference_nozzle_mm = mixed_filament_reference_nozzle_mm(mf.component_a, mf.component_b, nozzle_diameters);
    const int apparent_b = MixedFilamentManager::apparent_mix_b_percent(base_b,
                                                                        mf.component_a_surface_offset,
                                                                        mf.component_b_surface_offset,
                                                                        float(reference_nozzle_mm));
    return { 100 - apparent_b, apparent_b };
}

static std::string mixed_filament_apparent_pair_summary(const MixedFilament               &mf,
                                                        const MixedFilamentPreviewSettings &preview_settings,
                                                        const std::vector<double>          &nozzle_diameters,
                                                        bool                                bias_mode_enabled)
{
    if (!mixed_filament_supports_bias_apparent_color(mf, preview_settings, bias_mode_enabled))
        return {};

    const int base_b = MixedFilamentConfigPanel::effective_local_z_preview_mix_b_percent(mf, preview_settings);
    const int base_a = 100 - base_b;
    const auto [apparent_a, apparent_b] =
        mixed_filament_apparent_pair_percentages(mf, preview_settings, nozzle_diameters, bias_mode_enabled);

    if (std::abs(mf.component_a_surface_offset - mf.component_b_surface_offset) > 1e-4f &&
        (apparent_a != base_a || apparent_b != base_b)) {
        std::ostringstream ss;
        ss << '~' << apparent_a << '/' << apparent_b;
        return ss.str();
    }

    std::ostringstream ss;
    ss << apparent_a << "%/" << apparent_b << '%';
    return ss.str();
}

std::string MixedFilamentConfigPanel::summarize_sequence(const std::vector<unsigned int> &seq)
{
    if (seq.empty()) return "";
    std::unordered_map<unsigned int, int> counts;
    for (unsigned int id : seq) counts[id]++;
    std::vector<std::pair<int, unsigned int>> sorted;
    for (auto &kv : counts) sorted.emplace_back(kv.second, kv.first);
    std::sort(sorted.begin(), sorted.end(), std::greater<>());
    std::string out;
    for (auto &p : sorted) {
        if (!out.empty()) out += "/";
        out += format_preview_sequence_percent(p.first, int(seq.size()));
    }
    return out;
}

std::string MixedFilamentConfigPanel::summarize_local_z_breakdown(const MixedFilament &mf,
                                                                 const std::vector<int> &weights,
                                                                 const MixedFilamentPreviewSettings &preview_settings)
{
    const std::string normalized_pattern = MixedFilamentManager::normalize_manual_pattern(mf.manual_pattern);
    if (!normalized_pattern.empty())
        return "Local-Z breakdown: manual pattern rows do not use pair decomposition.";

    if (mf.distribution_mode == int(MixedFilament::SameLayerPointillisme))
        return "Local-Z breakdown: same-layer mode does not use local-Z pair decomposition.";

    auto pair_name = [](unsigned int a, unsigned int b) {
        std::ostringstream ss;
        ss << 'F' << a << "+F" << b;
        return ss.str();
    };
    auto pair_split = [](unsigned int a, unsigned int b, int weight_a, int weight_b) {
        const int safe_a = std::max(0, weight_a);
        const int safe_b = std::max(0, weight_b);
        const int total  = std::max(1, safe_a + safe_b);
        const int pct_a  = int(std::lround(100.0 * double(safe_a) / double(total)));
        const int pct_b  = std::max(0, 100 - pct_a);

        std::ostringstream ss;
        ss << 'F' << a << "/F" << b << " " << safe_a << ':' << safe_b << " (" << pct_a << '/' << pct_b << ')';
        return ss.str();
    };
    auto cadence_entry = [&pair_name](unsigned int a, unsigned int b, int weight, int total) {
        const int pct = int(std::lround(100.0 * double(std::max(0, weight)) / double(std::max(1, total))));
        std::ostringstream ss;
        ss << pair_name(a, b) << ' ' << pct << '%';
        return ss.str();
    };

    const std::vector<unsigned int> ids = decode_gradient_ids(mf.gradient_component_ids);
    if (ids.size() >= 4) {
        const std::vector<int> normalized = normalize_gradient_weights(weights, ids.size());
        const std::vector<unsigned int> pair_tokens = { 1, 2 };
        const std::vector<int> pair_weights = {
            std::max(1, normalized[0] + normalized[1]),
            std::max(1, normalized[2] + normalized[3])
        };
        const size_t max_pair_layers =
            (preview_settings.local_z_mode && mf.local_z_max_sublayers >= 2) ?
                std::max<size_t>(1, size_t(mf.local_z_max_sublayers) / 2) :
                size_t(0);
        const std::vector<unsigned int> uncapped_pair_sequence = build_weighted_multi_sequence(pair_tokens, pair_weights);
        const std::vector<unsigned int> effective_pair_sequence =
            max_pair_layers > 0 ? build_weighted_multi_sequence(pair_tokens, pair_weights, max_pair_layers) : uncapped_pair_sequence;
        const std::vector<unsigned int> &pair_sequence = effective_pair_sequence.empty() ? uncapped_pair_sequence : effective_pair_sequence;
        const int pair_ab_weight = int(std::count(pair_sequence.begin(), pair_sequence.end(), 1u));
        const int pair_cd_weight = int(std::count(pair_sequence.begin(), pair_sequence.end(), 2u));
        const int pair_total = std::max(1, int(pair_sequence.size()));

        std::ostringstream ss;
        ss << "Local-Z layer cadence: "
           << cadence_entry(ids[0], ids[1], pair_ab_weight, pair_total)
           << ", "
           << cadence_entry(ids[2], ids[3], pair_cd_weight, pair_total)
           << ".\nPair splits: "
           << pair_split(ids[0], ids[1], normalized[0], normalized[1])
           << ", "
           << pair_split(ids[2], ids[3], normalized[2], normalized[3])
           << '.';
        if (!preview_settings.local_z_mode && mf.local_z_max_sublayers >= 2)
            ss << "\nSaved row limit will apply when Local-Z dithering mode is enabled in print settings.";
        if (preview_settings.local_z_mode && mf.local_z_max_sublayers >= 2) {
            ss << "\nEffective Local-Z stack: " << (pair_total * 2) << " sublayers over " << pair_total << " pair layers";
            if (uncapped_pair_sequence.size() > pair_sequence.size())
                ss << " (uncapped " << (uncapped_pair_sequence.size() * 2) << ')';
            ss << '.';
        }
        return ss.str();
    }

    if (ids.size() == 3) {
        const std::vector<int> normalized = normalize_gradient_weights(weights, ids.size());
        const std::vector<unsigned int> pair_tokens = { 1, 2, 3 };
        const std::vector<int> pair_weights = {
            std::max(1, normalized[0] + normalized[1]),
            std::max(1, normalized[0] + normalized[2]),
            std::max(1, normalized[1] + normalized[2])
        };
        const size_t max_pair_layers =
            (preview_settings.local_z_mode && mf.local_z_max_sublayers >= 2) ?
                std::max<size_t>(1, size_t(mf.local_z_max_sublayers) / 2) :
                size_t(0);
        const std::vector<unsigned int> uncapped_pair_sequence = build_weighted_multi_sequence(pair_tokens, pair_weights);
        const std::vector<unsigned int> effective_pair_sequence =
            max_pair_layers > 0 ? build_weighted_multi_sequence(pair_tokens, pair_weights, max_pair_layers) : uncapped_pair_sequence;
        const std::vector<unsigned int> &pair_sequence = effective_pair_sequence.empty() ? uncapped_pair_sequence : effective_pair_sequence;
        const int pair_ab_weight = int(std::count(pair_sequence.begin(), pair_sequence.end(), 1u));
        const int pair_ac_weight = int(std::count(pair_sequence.begin(), pair_sequence.end(), 2u));
        const int pair_bc_weight = int(std::count(pair_sequence.begin(), pair_sequence.end(), 3u));
        const int pair_total     = std::max(1, int(pair_sequence.size()));

        std::ostringstream ss;
        ss << "Local-Z layer cadence: "
           << cadence_entry(ids[0], ids[1], pair_ab_weight, pair_total)
           << ", "
           << cadence_entry(ids[0], ids[2], pair_ac_weight, pair_total)
           << ", "
           << cadence_entry(ids[1], ids[2], pair_bc_weight, pair_total)
           << ".\nPair splits: "
           << pair_split(ids[0], ids[1], normalized[0], normalized[1])
           << ", "
           << pair_split(ids[0], ids[2], normalized[0], normalized[2])
           << ", "
           << pair_split(ids[1], ids[2], normalized[1], normalized[2])
           << '.';
        if (!preview_settings.local_z_mode && mf.local_z_max_sublayers >= 2)
            ss << "\nSaved row limit will apply when Local-Z dithering mode is enabled in print settings.";
        if (preview_settings.local_z_mode && mf.local_z_max_sublayers >= 2) {
            ss << "\nEffective Local-Z stack: " << (pair_total * 2) << " sublayers over " << pair_total << " pair layers";
            if (uncapped_pair_sequence.size() > pair_sequence.size())
                ss << " (uncapped " << (uncapped_pair_sequence.size() * 2) << ')';
            ss << '.';
        }
        return ss.str();
    }

    if (mf.component_a >= 1 && mf.component_b >= 1 && mf.component_a != mf.component_b) {
        const int pct_b = std::clamp(mf.mix_b_percent, 0, 100);
        const int pct_a = 100 - pct_b;
        std::ostringstream ss;
        ss << "Local-Z pair split: requested F" << mf.component_a << "/F" << mf.component_b
           << ' ' << pct_a << '/' << pct_b;
        if (preview_settings.local_z_mode) {
            const std::vector<double> effective_passes = build_local_z_preview_pass_heights(preview_settings.nominal_layer_height,
                                                                                            preview_settings.mixed_lower_bound,
                                                                                            preview_settings.mixed_upper_bound,
                                                                                            preview_settings.preferred_a_height,
                                                                                            preview_settings.preferred_b_height,
                                                                                            mf.mix_b_percent,
                                                                                            0);
            if (!effective_passes.empty()) {
                const int effective_pct_b = effective_local_z_preview_mix_b_percent(mf, preview_settings);
                ss << ", effective " << (100 - effective_pct_b) << '/' << effective_pct_b
                   << " over " << effective_passes.size() << " sublayers";
            }
        }
        ss << '.';
        return ss.str();
    }

    return "Local-Z breakdown: unavailable.";
}

std::string MixedFilamentConfigPanel::blend_from_sequence(const std::vector<std::string> &colors, const std::vector<unsigned int> &seq, const std::string &fallback)
{
    if (colors.empty() || seq.empty())
        return fallback;

    std::vector<size_t> counts(colors.size() + 1, size_t(0));
    size_t total = 0;
    for (const unsigned int id : seq) {
        if (id == 0 || id > colors.size())
            continue;
        ++counts[id];
        ++total;
    }
    if (total == 0)
        return fallback;

    unsigned int first_id = 0;
    for (size_t id = 1; id <= colors.size(); ++id) {
        if (counts[id] > 0) {
            first_id = unsigned(id);
            break;
        }
    }
    if (first_id == 0 || first_id > colors.size())
        return fallback;

    std::string blended = colors[first_id - 1];
    int acc = int(counts[first_id]);
    for (size_t id = size_t(first_id + 1); id <= colors.size(); ++id) {
        if (counts[id] == 0)
            continue;
        blended = MixedFilamentManager::blend_color(blended, colors[id - 1], acc, int(counts[id]));
        acc += int(counts[id]);
    }

    return blended;
}

MixedFilamentConfigPanel::MixedFilamentConfigPanel(wxWindow *parent,
                                                   size_t mixed_id,
                                                   const MixedFilament &mf,
                                                   size_t num_physical,
                                                   const std::vector<std::string> &physical_colors,
                                                   const std::vector<double> &nozzle_diameters,
                                                   const std::vector<wxColour> &palette,
                                                   const MixedFilamentPreviewSettings &preview_settings,
                                                   bool bias_mode_enabled,
                                                   OnChangeFn on_change)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE)
    , m_mixed_id(mixed_id)
    , m_mf(mf)
    , m_num_physical(num_physical)
    , m_physical_colors(physical_colors)
    , m_nozzle_diameters(nozzle_diameters)
    , m_palette(palette)
    , m_preview_settings(preview_settings)
    , m_bias_mode_enabled(bias_mode_enabled)
    , m_selected_weight_state(std::make_shared<std::vector<int>>())
    , m_on_change(on_change)
{
    if (parent)
        SetBackgroundColour(parent->GetBackgroundColour());
    else
        SetBackgroundColour(wxGetApp().dark_mode() ? wxColour(52, 52, 56) : wxColour(255, 255, 255));
    build_ui();
}

void MixedFilamentConfigPanel::build_ui()
{
    const int gap = FromDIP(6);
    const int compact_gap = std::max(FromDIP(2), gap / 3);
    const bool is_dark = wxGetApp().dark_mode();
    const wxColour panel_bg = GetBackgroundColour().IsOk() ? GetBackgroundColour() :
        (is_dark ? wxColour(52, 52, 56) : wxColour(255, 255, 255));
    SetBackgroundColour(panel_bg);
    auto *root = new wxBoxSizer(wxVERTICAL);

    // Filament choices
    wxArrayString filament_choices;
    for (size_t i = 0; i < m_num_physical; ++i)
        filament_choices.Add(wxString::Format("F%d", int(i + 1)));
    wxArrayString optional_filament_choices;
    optional_filament_choices.Add(_L("None"));
    for (size_t i = 0; i < m_num_physical; ++i)
        optional_filament_choices.Add(wxString::Format("F%d", int(i + 1)));

    const int component_a = std::clamp(int(m_mf.component_a), 1, int(m_num_physical));
    const int component_b = std::clamp(int(m_mf.component_b), 1, int(m_num_physical));

    const std::vector<unsigned int> initial_gradient_ids = decode_gradient_ids(m_mf.gradient_component_ids);
    const int stored_distribution_mode = std::clamp(m_mf.distribution_mode,
                                                    int(MixedFilament::LayerCycle),
                                                    int(MixedFilament::Simple));
    const int row_distribution_mode = initial_gradient_ids.size() >= 3 ?
        (stored_distribution_mode == int(MixedFilament::Simple) ? int(MixedFilament::LayerCycle) : stored_distribution_mode) :
        int(MixedFilament::Simple);
    m_mf.distribution_mode = row_distribution_mode;
    const bool multi_gradient_row = row_distribution_mode != int(MixedFilament::Simple) && initial_gradient_ids.size() >= 3;
    const int selection_c = initial_gradient_ids.size() >= 3 ? int(initial_gradient_ids[2]) : 0;
    const int selection_d = initial_gradient_ids.size() >= 4 ? int(initial_gradient_ids[3]) : 0;

    // Hidden data controls used as backing state for swatch pickers.
    m_choice_a = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, filament_choices);
    m_choice_b = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, filament_choices);
    m_choice_a->SetSelection(component_a - 1);
    m_choice_b->SetSelection(component_b - 1);
    m_choice_a->Hide();
    m_choice_b->Hide();
    if (multi_gradient_row) {
        m_choice_c = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, optional_filament_choices);
        m_choice_c->SetSelection(std::clamp(selection_c, 0, int(m_num_physical)));
        m_choice_c->Hide();
        if (initial_gradient_ids.size() >= 4) {
            m_choice_d = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, optional_filament_choices);
            m_choice_d->SetSelection(std::clamp(selection_d, 0, int(m_num_physical)));
            m_choice_d->Hide();
        }
    }

    auto create_component_picker = [this, gap](wxPanel *&container_out, wxPanel *&swatch_out, wxStaticText *&label_out, const wxString &tooltip) {
        const int inner_gap = std::max(FromDIP(1), gap / 4);
        const bool local_is_dark = wxGetApp().dark_mode();
        const wxColour local_picker_bg = local_is_dark ? wxColour(64, 64, 70) : wxColour(255, 255, 255);
        const wxColour local_picker_text = local_is_dark ? wxColour(230, 230, 230) : wxColour(32, 32, 32);
        container_out = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
        container_out->SetBackgroundColour(local_picker_bg);
        const wxSize picker_size(FromDIP(38), FromDIP(22));
        container_out->SetMinSize(picker_size);
        container_out->SetMaxSize(picker_size);

        auto *container_sizer = new wxBoxSizer(wxHORIZONTAL);
        swatch_out = new wxPanel(container_out, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(12), FromDIP(12)), wxBORDER_SIMPLE);
        swatch_out->SetMinSize(wxSize(FromDIP(12), FromDIP(12)));
        swatch_out->SetToolTip(tooltip);
        label_out = new wxStaticText(container_out, wxID_ANY, wxEmptyString);
        label_out->SetForegroundColour(local_picker_text);
        label_out->SetToolTip(tooltip);

        auto *content_sizer = new wxBoxSizer(wxHORIZONTAL);
        content_sizer->Add(swatch_out, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, inner_gap);
        content_sizer->Add(label_out, 0, wxALIGN_CENTER_VERTICAL);
        container_sizer->AddStretchSpacer(1);
        container_sizer->Add(content_sizer, 0, wxALIGN_CENTER_VERTICAL);
        container_sizer->AddStretchSpacer(1);
        container_out->SetSizer(container_sizer);
        container_out->SetToolTip(tooltip);
        container_out->SetCursor(wxCursor(wxCURSOR_HAND));
        swatch_out->SetCursor(wxCursor(wxCURSOR_HAND));
        label_out->SetCursor(wxCursor(wxCURSOR_HAND));
    };

    create_component_picker(m_picker_a_container, m_picker_a_swatch, m_picker_a_label, _L("Click to choose a physical filament color"));
    create_component_picker(m_picker_b_container, m_picker_b_swatch, m_picker_b_label, _L("Click to choose a physical filament color"));
    if (m_choice_c)
        create_component_picker(m_picker_c_container, m_picker_c_swatch, m_picker_c_label, _L("Click to choose a physical filament color"));
    if (m_choice_d)
        create_component_picker(m_picker_d_container, m_picker_d_swatch, m_picker_d_label, _L("Click to choose a physical filament color"));
    update_component_picker_visuals();

    // Check for pattern mode
    const std::string normalized_pattern = MixedFilamentManager::normalize_manual_pattern(m_mf.manual_pattern);
    const bool pattern_row_mode = !normalized_pattern.empty();

    auto *picker_row = new wxBoxSizer(wxHORIZONTAL);
    if (!pattern_row_mode) {
        auto add_picker = [this, picker_row, gap](wxPanel *container, bool &first_picker) {
            if (!container)
                return;
            if (!first_picker)
                picker_row->Add(new wxStaticText(this, wxID_ANY, "+"), 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, std::max(FromDIP(2), gap / 2));
            picker_row->Add(container, 0, wxALIGN_CENTER_VERTICAL);
            first_picker = false;
        };

        bool first_picker = true;
        add_picker(m_picker_a_container, first_picker);
        add_picker(m_picker_b_container, first_picker);
        add_picker(m_picker_c_container, first_picker);
        add_picker(m_picker_d_container, first_picker);
    } else {
        if (m_picker_a_container) m_picker_a_container->Hide();
        if (m_picker_b_container) m_picker_b_container->Hide();
        if (m_picker_c_container) m_picker_c_container->Hide();
        if (m_picker_d_container) m_picker_d_container->Hide();
    }
    root->Add(picker_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, gap);

    // Pattern controls (if pattern mode)
    if (pattern_row_mode) {
        auto *pattern_row = new wxBoxSizer(wxHORIZONTAL);
        auto *pattern_label = new wxStaticText(this, wxID_ANY, _L("Pattern"));
        pattern_label->SetForegroundColour(is_dark ? wxColour(236, 236, 236) : wxColour(20, 20, 20));
        pattern_row->Add(pattern_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
        m_pattern_ctrl = new wxTextCtrl(this, wxID_ANY, from_u8(normalized_pattern), wxDefaultPosition,
                                        wxSize(FromDIP(200), -1), wxTE_PROCESS_ENTER);
        m_pattern_ctrl->SetToolTip(_L("Manual repeating pattern. Use 1/2 or A/B for component A/B, "
                                      "and 3..9 for direct physical filament IDs. "
                                      "Use commas to define deeper perimeter patterns, for example 12,21. "
                                      "Example: 1/1/1/1/2/2/2/2, 12,21, or 1/2/3/4."));
        pattern_row->Add(m_pattern_ctrl, 1, wxALIGN_CENTER_VERTICAL);
        root->Add(pattern_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, gap);

        auto *quick_buttons = new wxBoxSizer(wxHORIZONTAL);
        for (size_t fid = 0; fid < m_num_physical; ++fid) {
            wxButton *btn = new wxButton(this, wxID_ANY, wxString::Format("%d", int(fid + 1)),
                                         wxDefaultPosition, wxSize(FromDIP(24), FromDIP(22)), wxBU_EXACTFIT);
            const wxColour chip_color = (fid < m_palette.size()) ? m_palette[fid] : wxColour("#26A69A");
            btn->SetBackgroundColour(chip_color);
            btn->SetToolTip(wxString::Format(_L("Append filament %d to pattern"), int(fid + 1)));
            quick_buttons->Add(btn, 0, wxRIGHT, FromDIP(4));
            m_pattern_quick_buttons.emplace_back(btn);
        }
        auto *filaments_label = new wxStaticText(this, wxID_ANY, _L("Filaments"));
        filaments_label->SetForegroundColour(is_dark ? wxColour(236, 236, 236) : wxColour(20, 20, 20));
        picker_row->Add(filaments_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, std::max(FromDIP(3), gap / 2));
        picker_row->Add(quick_buttons, 0, wxALIGN_CENTER_VERTICAL);
    } else {
        // Blend selector for non-pattern mode
        const bool simple_mode = row_distribution_mode == int(MixedFilament::Simple);
        std::vector<unsigned int> selected_gradient_ids = simple_mode ? std::vector<unsigned int>() : initial_gradient_ids;
        if (selected_gradient_ids.size() < 3) selected_gradient_ids.clear();
        if (selected_gradient_ids.empty()) {
            selected_gradient_ids.emplace_back(unsigned(component_a));
            if (component_b != component_a) selected_gradient_ids.emplace_back(unsigned(component_b));
        }
        const bool multi_gradient_mode = selected_gradient_ids.size() >= 3;
        *m_selected_weight_state = normalize_gradient_weights(
            decode_gradient_weights(m_mf.gradient_component_weights, selected_gradient_ids.size()),
            selected_gradient_ids.size());

        wxColour color_a = (component_a >= 1 && component_a <= int(m_palette.size())) ? m_palette[component_a - 1] : wxColour("#26A69A");
        wxColour color_b = (component_b >= 1 && component_b <= int(m_palette.size())) ? m_palette[component_b - 1] : wxColour("#26A69A");
        m_blend_selector = new MixedGradientSelector(this, color_a, color_b, std::clamp(m_mf.mix_b_percent, 0, 100));
        m_blend_selector->SetBackgroundColour(panel_bg);
        const bool same_layer_mode = row_distribution_mode == int(MixedFilament::SameLayerPointillisme);
        m_blend_label = nullptr;
        picker_row->AddSpacer(gap);
        picker_row->Add(m_blend_selector, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL | wxLEFT, gap);

        if (m_blend_selector) {
            std::vector<wxColour> corner_colors;
            corner_colors.reserve(selected_gradient_ids.size());
            for (const unsigned int id : selected_gradient_ids) {
                if (id >= 1 && id <= m_palette.size())
                    corner_colors.emplace_back(m_palette[id - 1]);
            }
            if (!simple_mode && corner_colors.size() >= 3)
                m_blend_selector->set_multi_preview(corner_colors, *m_selected_weight_state);
        }
    }

    // Preview
    auto *preview_row = new wxBoxSizer(wxHORIZONTAL);
    m_mix_preview = new MixedMixPreview(this);
    m_mix_preview->SetBackgroundColour(panel_bg);
    preview_row->Add(m_mix_preview, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL | wxRIGHT, compact_gap);

    auto *bias_controls = new wxBoxSizer(wxHORIZONTAL);
    const float initial_surface_offset_value = mixed_filament_single_surface_offset_value(m_mf, m_nozzle_diameters);
    const double initial_bias_limit = mixed_filament_bias_limit_mm(m_mf, m_nozzle_diameters);
    const wxString bias_tooltip =
        _L("Positive bias recesses the second filament in the pair; negative bias recesses the first filament.\n\n"
           "The color chip shows which filament the current value affects.\n\n"
           "Grouped wall patterns, same-layer pointillisme, and Local-Z dithering ignore it.");

    auto *surface_offset_label = new wxStaticText(this, wxID_ANY, _L("Bias"));
    surface_offset_label->SetForegroundColour(is_dark ? wxColour(236, 236, 236) : wxColour(20, 20, 20));
    surface_offset_label->SetToolTip(bias_tooltip);
    bias_controls->Add(surface_offset_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, compact_gap);

    create_component_picker(m_surface_offset_target_container,
                            m_surface_offset_target_swatch,
                            m_surface_offset_target_label,
                            bias_tooltip);
    if (m_surface_offset_target_container)
        m_surface_offset_target_container->SetCursor(wxCursor(wxCURSOR_ARROW));
    if (m_surface_offset_target_swatch)
        m_surface_offset_target_swatch->SetCursor(wxCursor(wxCURSOR_ARROW));
    if (m_surface_offset_target_label)
        m_surface_offset_target_label->SetCursor(wxCursor(wxCURSOR_ARROW));
    bias_controls->Add(m_surface_offset_target_container, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, compact_gap);

    m_surface_offset_spin = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(58), -1),
                                                 wxSP_ARROW_KEYS | wxALIGN_RIGHT | wxTE_PROCESS_ENTER,
                                                 -initial_bias_limit, initial_bias_limit,
                                                 std::clamp(double(initial_surface_offset_value), -initial_bias_limit, initial_bias_limit), 0.001);
    m_surface_offset_spin->SetDigits(3);
    m_surface_offset_spin->SetToolTip(bias_tooltip);
    bias_controls->Add(m_surface_offset_spin, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, compact_gap);

    auto *surface_offset_units = new wxStaticText(this, wxID_ANY, _L("mm"));
    surface_offset_units->SetForegroundColour(is_dark ? wxColour(210, 210, 210) : wxColour(72, 72, 72));
    surface_offset_units->SetToolTip(bias_tooltip);
    bias_controls->Add(surface_offset_units, 0, wxALIGN_CENTER_VERTICAL);
    if (m_bias_mode_enabled)
        preview_row->Add(bias_controls, 0, wxALIGN_CENTER_VERTICAL);
    else {
        surface_offset_label->Hide();
        if (m_surface_offset_target_container)
            m_surface_offset_target_container->Hide();
        if (m_surface_offset_spin)
            m_surface_offset_spin->Hide();
        surface_offset_units->Hide();
    }
    root->Add(preview_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, gap);

    if (m_bias_mode_enabled) {
        const auto initial_surface_offset_pair =
            mixed_filament_single_surface_offset_pair(m_mf, initial_surface_offset_value, m_nozzle_diameters);
        m_mf.component_a_surface_offset = initial_surface_offset_pair.first;
        m_mf.component_b_surface_offset = initial_surface_offset_pair.second;
    }

    const bool initial_component_surface_offsets_supported = m_bias_mode_enabled &&
                                                             !pattern_row_mode &&
                                                             row_distribution_mode != int(MixedFilament::SameLayerPointillisme) &&
                                                             !m_preview_settings.local_z_mode;
    if (m_surface_offset_spin)
        m_surface_offset_spin->Enable(initial_component_surface_offsets_supported);

    const bool local_z_limit_supported = multi_gradient_row &&
                                         row_distribution_mode != int(MixedFilament::SameLayerPointillisme);
    if (local_z_limit_supported) {
        auto *local_z_limit_row = new wxBoxSizer(wxHORIZONTAL);
        m_local_z_limit_checkbox = new wxCheckBox(this, wxID_ANY, _L("Limit Local-Z"));
        m_local_z_limit_checkbox->SetValue(m_mf.local_z_max_sublayers >= 2);
        m_local_z_limit_checkbox->SetForegroundColour(is_dark ? wxColour(236, 236, 236) : wxColour(20, 20, 20));
        m_local_z_limit_checkbox->SetToolTip(
            _L("Store a per-color Local-Z cadence cap. It applies when Local-Z dithering mode is enabled in print settings."));
        local_z_limit_row->Add(m_local_z_limit_checkbox, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);

        auto *local_z_limit_label = new wxStaticText(this, wxID_ANY, _L("Max sublayers"));
        local_z_limit_label->SetForegroundColour(is_dark ? wxColour(236, 236, 236) : wxColour(20, 20, 20));
        local_z_limit_row->Add(local_z_limit_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, std::max(FromDIP(3), gap / 2));

        const int initial_local_z_limit = std::max(2, m_mf.local_z_max_sublayers > 0 ? m_mf.local_z_max_sublayers : 6);
        m_local_z_limit_spin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(72), -1),
                                              wxSP_ARROW_KEYS | wxALIGN_RIGHT | wxTE_PROCESS_ENTER, 2, 999, initial_local_z_limit);
        m_local_z_limit_spin->SetToolTip(
            _L("Maximum number of Local-Z sublayers this color may use before its cadence repeats."));
        local_z_limit_row->Add(m_local_z_limit_spin, 0, wxALIGN_CENTER_VERTICAL);

        const bool enable_local_z_limit_controls = m_local_z_limit_checkbox->GetValue();
        m_local_z_limit_spin->Enable(enable_local_z_limit_controls);
        root->Add(local_z_limit_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, gap);
    }

    m_breakdown_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_breakdown_label->SetForegroundColour(is_dark ? wxColour(210, 210, 210) : wxColour(72, 72, 72));
    m_breakdown_label->Wrap(FromDIP(360));
    root->Add(m_breakdown_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, gap);

    // Bind events
    auto apply_changes = [this]() {
        m_has_changes = true;

        double surface_offset_value = 0.0;
        if (m_surface_offset_spin) {
            surface_offset_value = m_surface_offset_spin->GetValue();
#if !defined(wxHAS_NATIVE_SPINCTRLDOUBLE)
            if (wxTextCtrl *text = m_surface_offset_spin->GetText()) {
                double parsed_value = 0.0;
                if (text->GetValue().ToDouble(&parsed_value))
                    surface_offset_value = parsed_value;
            }
#endif
        }

        int a = std::clamp(m_choice_a->GetSelection() + 1, 1, int(m_num_physical));
        int b = std::clamp(m_choice_b->GetSelection() + 1, 1, int(m_num_physical));
        if (a == b && m_num_physical > 1) {
            b = (a == int(m_num_physical)) ? 1 : a + 1;
            m_choice_b->SetSelection(b - 1);
        }
        update_component_picker_visuals();

        if (m_local_z_limit_spin)
            m_local_z_limit_spin->Enable(m_local_z_limit_checkbox != nullptr &&
                                         m_local_z_limit_checkbox->GetValue());

        const bool preserve_same_layer_mode = m_mf.distribution_mode == int(MixedFilament::SameLayerPointillisme);
        m_mf.component_a = unsigned(a);
        m_mf.component_b = unsigned(b);
        if (m_bias_mode_enabled) {
            const double bias_limit = mixed_filament_bias_limit_mm(m_mf, m_nozzle_diameters);
            const float clamped_surface_offset_value = std::clamp(float(surface_offset_value), -float(bias_limit), float(bias_limit));
            const auto surface_offset_pair =
                mixed_filament_single_surface_offset_pair(m_mf, clamped_surface_offset_value, m_nozzle_diameters);
            m_mf.component_a_surface_offset = surface_offset_pair.first;
            m_mf.component_b_surface_offset = surface_offset_pair.second;
            if (m_surface_offset_spin)
                m_surface_offset_spin->SetValue(clamped_surface_offset_value);
        }
        m_mf.local_z_max_sublayers =
            (m_local_z_limit_checkbox != nullptr && m_local_z_limit_checkbox->GetValue() && m_local_z_limit_spin != nullptr) ?
                std::max(2, m_local_z_limit_spin->GetValue()) :
                0;

        bool simple_mode = true;
        bool same_layer_mode = false;
        int preview_mix_b_percent = std::clamp(m_mf.mix_b_percent, 0, 100);
        std::vector<unsigned int> preview_sequence;

        if (m_pattern_ctrl) {
            m_mf.distribution_mode = int(MixedFilament::Simple);
            std::string normalized = MixedFilamentManager::normalize_manual_pattern(into_u8(m_pattern_ctrl->GetValue()));
            if (normalized.empty()) normalized = "12";
            if (into_u8(m_pattern_ctrl->GetValue()) != normalized)
                m_pattern_ctrl->ChangeValue(from_u8(normalized));
            m_mf.manual_pattern = normalized;
            m_mf.mix_b_percent = MixedFilamentManager::mix_percent_from_manual_pattern(normalized);
            m_mf.pointillism_all_filaments = false;
            m_mf.gradient_component_ids.clear();
            m_mf.gradient_component_weights.clear();
            preview_sequence = decode_manual_pattern_ids(m_mf.manual_pattern,
                                                         m_mf.component_a,
                                                         m_mf.component_b,
                                                         m_num_physical,
                                                         m_preview_settings.wall_loops);
        } else {
            std::vector<unsigned int> selected_ids;
            selected_ids.reserve(4);
            auto add_unique = [&selected_ids](unsigned int id) {
                if (id == 0) return;
                if (std::find(selected_ids.begin(), selected_ids.end(), id) == selected_ids.end())
                    selected_ids.emplace_back(id);
            };
            add_unique(unsigned(a));
            add_unique(unsigned(b));
            if (m_choice_c && m_choice_c->GetSelection() > 0)
                add_unique(unsigned(m_choice_c->GetSelection()));
            if (m_choice_d && m_choice_d->GetSelection() > 0)
                add_unique(unsigned(m_choice_d->GetSelection()));
            const bool multi_gradient_mode = selected_ids.size() >= 3;
            m_mf.distribution_mode = multi_gradient_mode ?
                (preserve_same_layer_mode ? int(MixedFilament::SameLayerPointillisme) : int(MixedFilament::LayerCycle)) :
                int(MixedFilament::Simple);
            simple_mode = m_mf.distribution_mode == int(MixedFilament::Simple);
            same_layer_mode = m_mf.distribution_mode == int(MixedFilament::SameLayerPointillisme);
            m_mf.mix_b_percent = std::clamp(m_blend_selector ? m_blend_selector->value() : 50, 0, 100);
            m_mf.manual_pattern.clear();
            m_mf.pointillism_all_filaments = false;

            const wxColour color_a = (a >= 1 && a <= int(m_palette.size())) ? m_palette[size_t(a - 1)] : wxColour("#26A69A");
            const wxColour color_b = (b >= 1 && b <= int(m_palette.size())) ? m_palette[size_t(b - 1)] : wxColour("#26A69A");
            if (m_blend_selector) {
                if (!simple_mode && multi_gradient_mode) {
                    std::vector<wxColour> corner_colors;
                    corner_colors.reserve(selected_ids.size());
                    for (const unsigned int id : selected_ids) {
                        if (id >= 1 && id <= m_palette.size())
                            corner_colors.emplace_back(m_palette[id - 1]);
                    }
                    if (corner_colors.size() >= 3)
                        m_blend_selector->set_multi_preview(corner_colors, *m_selected_weight_state);
                    else
                        m_blend_selector->set_colors(color_a, color_b);
                } else {
                    m_blend_selector->set_colors(color_a, color_b);
                }
            }

            if (multi_gradient_mode) {
                const std::vector<int> decoded_weights =
                    decode_gradient_weights(m_mf.gradient_component_weights, selected_ids.size());
                if (m_selected_weight_state->size() != selected_ids.size())
                    *m_selected_weight_state = decoded_weights;
                *m_selected_weight_state = normalize_gradient_weights(*m_selected_weight_state, selected_ids.size());
                m_mf.gradient_component_ids = encode_gradient_ids(selected_ids);
                m_mf.gradient_component_weights = encode_gradient_weights(*m_selected_weight_state);
                preview_sequence = build_weighted_multi_sequence(selected_ids, *m_selected_weight_state);
            } else {
                m_mf.gradient_component_ids.clear();
                m_mf.gradient_component_weights.clear();
                preview_mix_b_percent = effective_local_z_preview_mix_b_percent(m_mf, m_preview_settings);
                preview_sequence = build_weighted_pair_sequence(m_mf.component_a, m_mf.component_b, preview_mix_b_percent, same_layer_mode);
            }
        }
        m_mf.custom = true;

        const std::vector<unsigned int> selected_gradient_ids = decode_gradient_ids(m_mf.gradient_component_ids);
        const bool component_surface_offsets_supported = m_bias_mode_enabled &&
                                                         (m_pattern_ctrl == nullptr) &&
                                                         !same_layer_mode &&
                                                         !m_preview_settings.local_z_mode;
        if (m_surface_offset_spin)
            m_surface_offset_spin->Enable(component_surface_offsets_supported);
        if (preview_sequence.empty())
            preview_sequence = build_weighted_pair_sequence(m_mf.component_a, m_mf.component_b, preview_mix_b_percent, same_layer_mode);

        if (m_blend_selector && selected_gradient_ids.size() >= 3) {
            std::vector<wxColour> corner_colors;
            corner_colors.reserve(selected_gradient_ids.size());
            for (const unsigned int id : selected_gradient_ids) {
                if (id >= 1 && id <= m_palette.size())
                    corner_colors.emplace_back(m_palette[id - 1]);
            }
            if (corner_colors.size() >= 3)
                m_blend_selector->set_multi_preview(corner_colors, *m_selected_weight_state);
        }

        if (mixed_filament_supports_bias_apparent_color(m_mf, m_preview_settings, m_bias_mode_enabled) &&
            m_mf.component_a >= 1 && m_mf.component_b >= 1 &&
            m_mf.component_a <= m_physical_colors.size() && m_mf.component_b <= m_physical_colors.size()) {
            const auto [apparent_pct_a, apparent_pct_b] =
                mixed_filament_apparent_pair_percentages(m_mf, m_preview_settings, m_nozzle_diameters, m_bias_mode_enabled);
            m_mf.display_color = MixedFilamentManager::blend_color(
                m_physical_colors[size_t(m_mf.component_a - 1)],
                m_physical_colors[size_t(m_mf.component_b - 1)],
                apparent_pct_a,
                apparent_pct_b);
        } else if (selected_gradient_ids.size() >= 3 || !preview_sequence.empty()) {
            m_mf.display_color = blend_from_sequence(m_physical_colors, preview_sequence, "#26A69A");
            if (m_blend_label) {
                if (selected_gradient_ids.size() >= 3) {
                    m_blend_label->SetLabel(wxString::Format(same_layer_mode ? _L("%d-color pointillisme") : _L("%d-color layer cycle"),
                                                            int(selected_gradient_ids.size())));
                } else {
                    m_blend_label->SetLabel(wxString::Format(simple_mode ? _L("Simple %d%%/%d%%") :
                                                               (same_layer_mode ? _L("Pointillisme %d%%/%d%%") : _L("%d%%/%d%%")),
                                                               100 - preview_mix_b_percent, preview_mix_b_percent));
                }
            }
        } else {
            m_mf.display_color = MixedFilamentManager::blend_color(
                m_physical_colors[size_t(a - 1)], m_physical_colors[size_t(b - 1)],
                100 - preview_mix_b_percent, preview_mix_b_percent);
            if (m_blend_label)
                m_blend_label->SetLabel(wxString::Format(simple_mode ? _L("Simple %d%%/%d%%") :
                                                           (same_layer_mode ? _L("Pointillisme %d%%/%d%%") : _L("%d%%/%d%%")),
                                                           100 - preview_mix_b_percent, preview_mix_b_percent));
        }

        if (m_mix_preview) {
            const std::string bias_summary =
                mixed_filament_apparent_pair_summary(m_mf, m_preview_settings, m_nozzle_diameters, m_bias_mode_enabled);
            const std::string summary = bias_summary.empty() ? summarize_sequence(preview_sequence) : bias_summary;
            std::vector<double> preview_surface_offsets(m_palette.size() + 1, 0.0);
            if (m_bias_mode_enabled && m_mf.component_a >= 1 && m_mf.component_a < preview_surface_offsets.size())
                preview_surface_offsets[m_mf.component_a] = double(m_mf.component_a_surface_offset);
            if (m_bias_mode_enabled && m_mf.component_b >= 1 && m_mf.component_b < preview_surface_offsets.size())
                preview_surface_offsets[m_mf.component_b] = double(m_mf.component_b_surface_offset);
            m_mix_preview->set_data(m_palette, preview_sequence, same_layer_mode, preview_surface_offsets, wxColour(m_mf.display_color),
                                    _L("Preview"), summary.empty() ? wxString() : from_u8(summary));
        }
        update_local_z_breakdown();
        if (m_swatch) {
            m_swatch->SetBackgroundColour(wxColour(m_mf.display_color));
            m_swatch->Refresh();
        }
        if (m_on_change)
            m_on_change(m_mf);
    };

    auto make_color_chip_bitmap = [this](const wxColour &color) {
        const int chip_size = FromDIP(14);
        wxBitmap bmp(chip_size, chip_size);
        wxMemoryDC dc(bmp);
        dc.SetBackground(wxBrush(wxColour(255, 255, 255)));
        dc.Clear();
        dc.SetPen(wxPen(wxColour(120, 120, 120)));
        dc.SetBrush(wxBrush(color));
        dc.DrawRectangle(0, 0, chip_size, chip_size);
        dc.SelectObject(wxNullBitmap);
        return bmp;
    };

    auto bind_component_picker_popup = [this, apply_changes, make_color_chip_bitmap](wxWindow *target, wxChoice *backing_choice) {
        if (!target || !backing_choice)
            return;

        target->Bind(wxEVT_LEFT_UP, [this, apply_changes, make_color_chip_bitmap, backing_choice](wxMouseEvent &) {
            if (m_num_physical == 0)
                return;

            const bool allow_none = backing_choice->GetCount() == unsigned(m_num_physical + 1);
            wxMenu menu;
            std::vector<int> item_ids;
            item_ids.reserve(m_num_physical + (allow_none ? 1 : 0));
            if (allow_none) {
                const int item_id = wxWindow::NewControlId();
                item_ids.emplace_back(item_id);
                menu.Append(item_id, backing_choice->GetSelection() == 0 ? _L("None (Selected)") : _L("None"));
            }
            for (size_t i = 0; i < m_num_physical; ++i) {
                const int item_id = wxWindow::NewControlId();
                item_ids.emplace_back(item_id);
                const int selection_index = allow_none ? int(i + 1) : int(i);
                const bool is_selected = selection_index == backing_choice->GetSelection();
                const wxString item_label = wxString::Format("F%d%s", int(i + 1), is_selected ? " (Selected)" : "");
                auto *menu_item = new wxMenuItem(&menu, item_id, item_label, wxEmptyString, wxITEM_NORMAL);
                const wxColour item_color = (i < m_palette.size()) ? m_palette[i] : wxColour("#26A69A");
                menu_item->SetBitmap(make_color_chip_bitmap(item_color));
                menu.Append(menu_item);
            }

            menu.Bind(wxEVT_COMMAND_MENU_SELECTED, [apply_changes, backing_choice, item_ids](wxCommandEvent &evt) {
                const auto it = std::find(item_ids.begin(), item_ids.end(), evt.GetId());
                if (it == item_ids.end())
                    return;
                const int selection = int(std::distance(item_ids.begin(), it));
                backing_choice->SetSelection(selection);
                apply_changes();
            });
            PopupMenu(&menu);
        });
    };

    bind_component_picker_popup(m_picker_a_container, m_choice_a);
    bind_component_picker_popup(m_picker_a_swatch, m_choice_a);
    bind_component_picker_popup(m_picker_a_label, m_choice_a);
    bind_component_picker_popup(m_picker_b_container, m_choice_b);
    bind_component_picker_popup(m_picker_b_swatch, m_choice_b);
    bind_component_picker_popup(m_picker_b_label, m_choice_b);
    bind_component_picker_popup(m_picker_c_container, m_choice_c);
    bind_component_picker_popup(m_picker_c_swatch, m_choice_c);
    bind_component_picker_popup(m_picker_c_label, m_choice_c);
    bind_component_picker_popup(m_picker_d_container, m_choice_d);
    bind_component_picker_popup(m_picker_d_swatch, m_choice_d);
    bind_component_picker_popup(m_picker_d_label, m_choice_d);

    m_choice_a->Bind(wxEVT_CHOICE, [apply_changes](wxCommandEvent&) { apply_changes(); });
    m_choice_b->Bind(wxEVT_CHOICE, [apply_changes](wxCommandEvent&) { apply_changes(); });
    if (m_choice_c)
        m_choice_c->Bind(wxEVT_CHOICE, [apply_changes](wxCommandEvent&) { apply_changes(); });
    if (m_choice_d)
        m_choice_d->Bind(wxEVT_CHOICE, [apply_changes](wxCommandEvent&) { apply_changes(); });
    if (m_blend_selector)
        m_blend_selector->Bind(wxEVT_SLIDER, [apply_changes](wxCommandEvent&) { apply_changes(); });
    if (m_local_z_limit_checkbox)
        m_local_z_limit_checkbox->Bind(wxEVT_CHECKBOX, [apply_changes](wxCommandEvent &) { apply_changes(); });
    if (m_local_z_limit_spin) {
        m_local_z_limit_spin->Bind(wxEVT_SPINCTRL, [apply_changes](wxCommandEvent &) { apply_changes(); });
        m_local_z_limit_spin->Bind(wxEVT_TEXT_ENTER, [apply_changes](wxCommandEvent &) { apply_changes(); });
        m_local_z_limit_spin->Bind(wxEVT_KILL_FOCUS, [apply_changes](wxFocusEvent &evt) {
            apply_changes();
            evt.Skip();
        });
    }
    if (m_surface_offset_spin) {
        m_surface_offset_spin->Bind(wxEVT_SPINCTRLDOUBLE, [apply_changes](wxSpinDoubleEvent &) { apply_changes(); });
        m_surface_offset_spin->Bind(wxEVT_TEXT_ENTER, [apply_changes](wxCommandEvent &) { apply_changes(); });
        m_surface_offset_spin->Bind(wxEVT_KILL_FOCUS, [apply_changes](wxFocusEvent &evt) {
            apply_changes();
            evt.Skip();
        });
    }

    if (m_blend_selector) {
        m_blend_selector->Bind(wxEVT_BUTTON, [this, apply_changes](wxCommandEvent&) {
            if (!m_blend_selector->is_multi_mode()) return;
            std::vector<unsigned int> selected_ids;
            auto add_unique = [&selected_ids](unsigned int id) { if (id > 0 && std::find(selected_ids.begin(), selected_ids.end(), id) == selected_ids.end()) selected_ids.emplace_back(id); };
            add_unique(unsigned(std::clamp(m_choice_a ? (m_choice_a->GetSelection() + 1) : 0, 1, int(m_num_physical))));
            add_unique(unsigned(std::clamp(m_choice_b ? (m_choice_b->GetSelection() + 1) : 0, 1, int(m_num_physical))));
            if (m_choice_c && m_choice_c->GetSelection() > 0) add_unique(unsigned(m_choice_c->GetSelection()));
            if (m_choice_d && m_choice_d->GetSelection() > 0) add_unique(unsigned(m_choice_d->GetSelection()));
            if (selected_ids.size() < 3) return;
            const std::vector<int> initial_weights = normalize_gradient_weights(*m_selected_weight_state, selected_ids.size());
            MixedGradientWeightsDialog dlg(this, selected_ids, m_palette, initial_weights);
            if (dlg.ShowModal() != wxID_OK) return;
            *m_selected_weight_state = dlg.normalized_weights();
            apply_changes();
        });
    }

    if (m_pattern_ctrl) {
        auto append_pattern_token = [this](int filament_id) {
            if (!m_pattern_ctrl || filament_id <= 0) return;
            std::string pattern = into_u8(m_pattern_ctrl->GetValue());
            if (!pattern.empty()) {
                const char last = pattern.back();
                const bool has_sep = last == '/' || last == '-' || last == '_' || last == '|' || last == ':' || last == ';' || last == ',' || last == ' ';
                if (!has_sep) pattern.push_back('/');
            }
            pattern += std::to_string(filament_id);
            m_pattern_ctrl->ChangeValue(from_u8(pattern));
        };
        m_pattern_ctrl->Bind(wxEVT_TEXT_ENTER, [apply_changes](wxCommandEvent&) { apply_changes(); });
        m_pattern_ctrl->Bind(wxEVT_KILL_FOCUS, [apply_changes](wxFocusEvent &evt) { apply_changes(); evt.Skip(); });
        for (size_t fid = 0; fid < m_pattern_quick_buttons.size(); ++fid) {
            wxButton *btn = m_pattern_quick_buttons[fid];
            if (btn) {
                const int filament_id = int(fid + 1);
                btn->Bind(wxEVT_BUTTON, [apply_changes, append_pattern_token, filament_id](wxCommandEvent&) {
                    append_pattern_token(filament_id);
                    apply_changes();
                });
            }
        }
    }

    update_component_picker_visuals();
    SetSizer(root);
    Layout();
    SetMinSize(wxSize(-1, GetBestSize().GetHeight()));
    update_preview();
}

void MixedFilamentConfigPanel::update_component_picker_visuals()
{
    auto update_one = [this](wxChoice *choice, wxPanel *container, wxPanel *swatch, wxStaticText *label) {
        if (!choice)
            return;
        int sel = choice->GetSelection();
        const bool allow_none = choice->GetCount() == unsigned(m_num_physical + 1);
        if (sel < 0 && m_num_physical > 0) {
            sel = 0;
            choice->SetSelection(sel);
        }
        if (sel < 0)
            return;

        if (allow_none && sel == 0) {
            const wxColour none_color = wxGetApp().dark_mode() ? wxColour(86, 86, 92) : wxColour(224, 224, 224);
            if (swatch) {
                swatch->SetBackgroundColour(none_color);
                swatch->Refresh();
            }
            if (label)
                label->SetLabel(_L("None"));
            if (container) {
                container->Layout();
                container->Refresh();
            }
            return;
        }

        const int color_idx = allow_none ? sel - 1 : sel;
        const wxColour color = (color_idx >= 0 && size_t(color_idx) < m_palette.size()) ? m_palette[size_t(color_idx)] : wxColour("#26A69A");
        if (swatch) {
            swatch->SetBackgroundColour(color);
            swatch->Refresh();
        }
        if (label)
            label->SetLabel(wxString::Format("F%d", color_idx + 1));
        if (container) {
            container->Layout();
            container->Refresh();
        }
    };

    update_one(m_choice_a, m_picker_a_container, m_picker_a_swatch, m_picker_a_label);
    update_one(m_choice_b, m_picker_b_container, m_picker_b_swatch, m_picker_b_label);
    update_one(m_choice_c, m_picker_c_container, m_picker_c_swatch, m_picker_c_label);
    update_one(m_choice_d, m_picker_d_container, m_picker_d_swatch, m_picker_d_label);

    if (m_surface_offset_target_container || m_surface_offset_target_swatch || m_surface_offset_target_label || m_surface_offset_spin) {
        const int a_filament = std::clamp(m_choice_a ? (m_choice_a->GetSelection() + 1) : int(m_mf.component_a), 1, int(std::max<size_t>(1, m_num_physical)));
        const int b_filament = std::clamp(m_choice_b ? (m_choice_b->GetSelection() + 1) : int(m_mf.component_b), 1, int(std::max<size_t>(1, m_num_physical)));
        MixedFilament active_pair = m_mf;
        active_pair.component_a = unsigned(a_filament);
        active_pair.component_b = unsigned(b_filament);
        double signed_bias_value = mixed_filament_single_surface_offset_value(active_pair, m_nozzle_diameters);

        if (m_surface_offset_spin && m_bias_mode_enabled) {
            const double bias_limit = mixed_filament_bias_limit_mm(active_pair, m_nozzle_diameters);
            m_surface_offset_spin->SetRange(-bias_limit, bias_limit);
            signed_bias_value = m_surface_offset_spin->GetValue();
        }

        const int active_filament = signed_bias_value < -EPSILON ? a_filament : b_filament;
        const int color_idx = active_filament - 1;
        const wxColour color = (color_idx >= 0 && size_t(color_idx) < m_palette.size()) ? m_palette[size_t(color_idx)] : wxColour("#26A69A");
        if (m_surface_offset_target_swatch) {
            m_surface_offset_target_swatch->SetBackgroundColour(color);
            m_surface_offset_target_swatch->Refresh();
        }
        if (m_surface_offset_target_label)
            m_surface_offset_target_label->SetLabel(wxString::Format("F%d", active_filament));
        if (m_surface_offset_target_container) {
            m_surface_offset_target_container->Layout();
            m_surface_offset_target_container->Refresh();
        }
    }
}

void MixedFilamentConfigPanel::update_preview()
{
    const bool simple_mode = m_mf.distribution_mode == int(MixedFilament::Simple);
    const bool same_layer_mode = m_mf.distribution_mode == int(MixedFilament::SameLayerPointillisme);
    const std::string normalized_pattern = MixedFilamentManager::normalize_manual_pattern(m_mf.manual_pattern);
    const bool pattern_row_mode = !normalized_pattern.empty();

    std::vector<unsigned int> initial_sequence;
    if (pattern_row_mode) {
        initial_sequence = decode_manual_pattern_ids(normalized_pattern,
                                                     m_mf.component_a,
                                                     m_mf.component_b,
                                                     m_num_physical,
                                                     m_preview_settings.wall_loops);
    } else {
        std::vector<unsigned int> initial_gradient_ids = simple_mode ? std::vector<unsigned int>() : decode_gradient_ids(m_mf.gradient_component_ids);
        if (initial_gradient_ids.size() >= 3)
            initial_sequence = build_weighted_multi_sequence(initial_gradient_ids, *m_selected_weight_state);
        else
            initial_sequence = build_weighted_pair_sequence(m_mf.component_a,
                                                            m_mf.component_b,
                                                            effective_local_z_preview_mix_b_percent(m_mf, m_preview_settings),
                                                            same_layer_mode);

        if (m_blend_selector && initial_gradient_ids.size() >= 3) {
            std::vector<wxColour> corner_colors;
            corner_colors.reserve(initial_gradient_ids.size());
            for (const unsigned int id : initial_gradient_ids) {
                if (id >= 1 && id <= m_palette.size())
                    corner_colors.emplace_back(m_palette[id - 1]);
            }
            if (corner_colors.size() >= 3)
                m_blend_selector->set_multi_preview(corner_colors, *m_selected_weight_state);
        }
    }

    if (m_mix_preview) {
        if (mixed_filament_supports_bias_apparent_color(m_mf, m_preview_settings, m_bias_mode_enabled) &&
            m_mf.component_a >= 1 && m_mf.component_b >= 1 &&
            m_mf.component_a <= m_physical_colors.size() && m_mf.component_b <= m_physical_colors.size()) {
            const auto [apparent_pct_a, apparent_pct_b] =
                mixed_filament_apparent_pair_percentages(m_mf, m_preview_settings, m_nozzle_diameters, m_bias_mode_enabled);
            m_mf.display_color = MixedFilamentManager::blend_color(
                m_physical_colors[size_t(m_mf.component_a - 1)],
                m_physical_colors[size_t(m_mf.component_b - 1)],
                apparent_pct_a,
                apparent_pct_b);
        }

        const std::string bias_summary =
            mixed_filament_apparent_pair_summary(m_mf, m_preview_settings, m_nozzle_diameters, m_bias_mode_enabled);
        const std::string summary = bias_summary.empty() ? summarize_sequence(initial_sequence) : bias_summary;
        std::vector<double> preview_surface_offsets(m_palette.size() + 1, 0.0);
        if (m_bias_mode_enabled && m_mf.component_a >= 1 && m_mf.component_a < preview_surface_offsets.size())
            preview_surface_offsets[m_mf.component_a] = double(m_mf.component_a_surface_offset);
        if (m_bias_mode_enabled && m_mf.component_b >= 1 && m_mf.component_b < preview_surface_offsets.size())
            preview_surface_offsets[m_mf.component_b] = double(m_mf.component_b_surface_offset);
        m_mix_preview->set_data(m_palette, initial_sequence, same_layer_mode, preview_surface_offsets, wxColour(m_mf.display_color),
                                _L("Preview"), summary.empty() ? wxString() : from_u8(summary));
    }
    update_local_z_breakdown();
}

void MixedFilamentConfigPanel::update_local_z_breakdown()
{
    if (!m_breakdown_label)
        return;

    std::vector<int> weights = *m_selected_weight_state;
    const std::vector<unsigned int> ids = decode_gradient_ids(m_mf.gradient_component_ids);
    if (!ids.empty())
        weights = normalize_gradient_weights(weights, ids.size());

    const std::string breakdown = summarize_local_z_breakdown(m_mf, weights, m_preview_settings);
    m_breakdown_label->SetLabel(from_u8(breakdown));
    m_breakdown_label->Wrap(FromDIP(360));
    m_breakdown_label->Show(!breakdown.empty());
    Layout();
}

class MixedFilamentDragHandle : public wxPanel
{
public:
    MixedFilamentDragHandle(wxWindow *parent, const wxColour &dot_color, const wxColour &bg_color)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
        , m_dot_color(dot_color)
    {
        const wxSize handle_size = parent ? parent->FromDIP(wxSize(14, 18)) : wxSize(14, 18);
        SetMinSize(handle_size);
        SetMaxSize(handle_size);
        SetInitialSize(handle_size);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(bg_color);
        SetCursor(wxCursor(wxCURSOR_SIZING));
        Bind(wxEVT_PAINT, &MixedFilamentDragHandle::on_paint, this);
    }

    void set_colors(const wxColour &dot_color, const wxColour &bg_color)
    {
        m_dot_color = dot_color;
        SetBackgroundColour(bg_color);
        Refresh();
    }

private:
    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(m_dot_color));

        const wxSize size = GetClientSize();
        const int    radius = std::max(1, FromDIP(1));
        const int    left_x = std::max(radius, size.x / 2 - FromDIP(2));
        const int    right_x = std::min(size.x - radius - 1, size.x / 2 + FromDIP(2));
        const int    top_y = std::max(radius + 1, size.y / 2 - FromDIP(5));
        const int    gap_y = FromDIP(4);

        for (int row = 0; row < 3; ++row) {
            const int y = top_y + row * gap_y;
            dc.DrawCircle(wxPoint(left_x, y), radius);
            dc.DrawCircle(wxPoint(right_x, y), radius);
        }
    }

    wxColour m_dot_color;
};

static std::vector<size_t> build_mixed_filament_ui_indices(const std::vector<MixedFilament> &mixed,
                                                           const std::vector<uint64_t>      &preferred_order)
{
    std::vector<size_t> ordered_indices;
    std::vector<bool>   used(mixed.size(), false);

    for (const uint64_t stable_id : preferred_order) {
        for (size_t idx = 0; idx < mixed.size(); ++idx) {
            const MixedFilament &entry = mixed[idx];
            if (used[idx] || entry.deleted || entry.stable_id != stable_id)
                continue;
            used[idx] = true;
            ordered_indices.emplace_back(idx);
            break;
        }
    }

    for (size_t idx = 0; idx < mixed.size(); ++idx) {
        if (used[idx] || mixed[idx].deleted)
            continue;
        ordered_indices.emplace_back(idx);
    }

    return ordered_indices;
}

} // namespace

MixedColorMatchRecipeResult prompt_best_color_match_recipe(wxWindow *parent,
                                                           const std::vector<std::string> &physical_colors,
                                                           const wxColour &initial_color)
{
    MixedFilamentColorMatchDialog dlg(parent, physical_colors, initial_color);
    dlg.begin_initial_recipe_load();
    if (dlg.ShowModal() != wxID_OK) {
        MixedColorMatchRecipeResult cancelled;
        cancelled.cancelled = true;
        return cancelled;
    }

    return dlg.selected_recipe();
}

void Sidebar::update_mixed_filament_panel(bool sync_manager)
{
    // Check for new collapsible structure
    if (!p->m_panel_mixed_filaments_title || !p->m_panel_mixed_filaments_content)
        return;

    wxWindowUpdateLocker noUpdates_sidebar(this);
    wxWindowUpdateLocker noUpdates_mixed_panel(p->m_panel_mixed_filaments_content);

    auto refresh_model_canvas_colors = []() {
        Plater *plater = wxGetApp().plater();
        if (plater == nullptr)
            return;

        auto refresh_canvas = [](GLCanvas3D *canvas) {
            if (canvas == nullptr || !canvas->is_initialized())
                return;
            canvas->update_volumes_colors_by_extruder();
            canvas->render();
        };

        refresh_canvas(plater->get_view3D_canvas3D());
        refresh_canvas(plater->get_assmeble_canvas3D());
    };

    int prev_rows_view_y = 0;
    for (wxWindow *child : p->m_panel_mixed_filaments_content->GetChildren()) {
        if (auto *scrolled = dynamic_cast<wxScrolledWindow*>(child)) {
            int tmp_x = 0;
            scrolled->GetViewStart(&tmp_x, &prev_rows_view_y);
            break;
        }
    }

    auto *preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle)
        return;
    DynamicPrintConfig *print_cfg = &preset_bundle->prints.get_edited_preset().config;

    const size_t num_physical = p->combos_filament.size();
    ConfigOptionStrings *color_opt = preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    std::vector<std::string> physical_colors = color_opt ? color_opt->values : std::vector<std::string>();
    physical_colors.resize(num_physical, "#26A69A");
    std::vector<double> nozzle_diameters(num_physical, 0.4);
    if (const ConfigOptionFloats *opt = preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")) {
        const size_t opt_count = opt->values.size();
        if (opt_count > 0) {
            for (size_t i = 0; i < num_physical; ++i)
                nozzle_diameters[i] = std::max(0.05, opt->get_at(unsigned(std::min(i, opt_count - 1))));
        }
    }

    auto get_mixed_bool = [preset_bundle, print_cfg](const std::string &key, bool fallback) {
        if (const ConfigOptionBool *opt = preset_bundle->project_config.option<ConfigOptionBool>(key))
            return opt->value;
        if (const ConfigOptionInt *opt = preset_bundle->project_config.option<ConfigOptionInt>(key))
            return opt->value != 0;
        if (print_cfg) {
            if (const ConfigOptionBool *opt = print_cfg->option<ConfigOptionBool>(key))
                return opt->value;
            if (const ConfigOptionInt *opt = print_cfg->option<ConfigOptionInt>(key))
                return opt->value != 0;
        }
        return fallback;
    };
    auto get_mixed_mode = [preset_bundle, print_cfg](bool fallback) {
        if (const ConfigOptionBool *opt = preset_bundle->project_config.option<ConfigOptionBool>("mixed_filament_gradient_mode"))
            return opt->value;
        if (const ConfigOptionInt *opt = preset_bundle->project_config.option<ConfigOptionInt>("mixed_filament_gradient_mode"))
            return opt->value != 0;
        if (print_cfg) {
            if (const ConfigOptionBool *opt = print_cfg->option<ConfigOptionBool>("mixed_filament_gradient_mode"))
                return opt->value;
            if (const ConfigOptionInt *opt = print_cfg->option<ConfigOptionInt>("mixed_filament_gradient_mode"))
                return opt->value != 0;
        }
        return fallback;
    };
    auto get_mixed_float = [preset_bundle, print_cfg](const std::string &key, float fallback) {
        if (preset_bundle->project_config.has(key))
            return float(preset_bundle->project_config.opt_float(key));
        if (print_cfg && print_cfg->has(key))
            return float(print_cfg->opt_float(key));
        return fallback;
    };
    auto get_mixed_string = [preset_bundle, print_cfg](const std::string &key, const std::string &fallback = std::string()) {
        std::string project_value;
        if (preset_bundle->project_config.has(key))
            project_value = preset_bundle->project_config.opt_string(key);
        if (!project_value.empty())
            return project_value;
        if (print_cfg && print_cfg->has(key)) {
            const std::string print_value = print_cfg->opt_string(key);
            if (!print_value.empty())
                return print_value;
        }
        return project_value.empty() ? fallback : project_value;
    };
    auto set_mixed_float = [preset_bundle, print_cfg](const std::string &key, float value) {
        if (print_cfg) {
            if (ConfigOptionFloat *opt = print_cfg->option<ConfigOptionFloat>(key))
                opt->value = value;
            else
                print_cfg->set_key_value(key, new ConfigOptionFloat(value));
        }
        if (ConfigOptionFloat *opt = preset_bundle->project_config.option<ConfigOptionFloat>(key))
            opt->value = value;
        else
            preset_bundle->project_config.set_key_value(key, new ConfigOptionFloat(value));
    };
    auto set_mixed_string = [preset_bundle, print_cfg](const std::string &key, const std::string &value) {
        if (print_cfg) {
            if (ConfigOptionString *opt = print_cfg->option<ConfigOptionString>(key))
                opt->value = value;
            else
                print_cfg->set_key_value(key, new ConfigOptionString(value));
        }
        if (ConfigOptionString *opt = preset_bundle->project_config.option<ConfigOptionString>(key))
            opt->value = value;
        else
            preset_bundle->project_config.set_key_value(key, new ConfigOptionString(value));
    };
    auto set_mixed_bool = [preset_bundle, print_cfg](const std::string &key, bool value) {
        if (print_cfg) {
            if (ConfigOptionBool *opt = print_cfg->option<ConfigOptionBool>(key))
                opt->value = value;
            else if (ConfigOptionInt *opt = print_cfg->option<ConfigOptionInt>(key))
                opt->value = value ? 1 : 0;
            else
                print_cfg->set_key_value(key, new ConfigOptionBool(value));
        }
        if (ConfigOptionBool *opt = preset_bundle->project_config.option<ConfigOptionBool>(key))
            opt->value = value;
        else if (ConfigOptionInt *opt = preset_bundle->project_config.option<ConfigOptionInt>(key))
            opt->value = value ? 1 : 0;
        else
            preset_bundle->project_config.set_key_value(key, new ConfigOptionBool(value));
    };
    auto set_mixed_mode = [preset_bundle, print_cfg](bool enabled) {
        if (print_cfg) {
            if (ConfigOptionBool *opt = print_cfg->option<ConfigOptionBool>("mixed_filament_gradient_mode"))
                opt->value = enabled;
            else if (ConfigOptionInt *opt = print_cfg->option<ConfigOptionInt>("mixed_filament_gradient_mode"))
                opt->value = enabled ? 1 : 0;
            else
                print_cfg->set_key_value("mixed_filament_gradient_mode", new ConfigOptionBool(enabled));
        }
        if (ConfigOptionBool *opt = preset_bundle->project_config.option<ConfigOptionBool>("mixed_filament_gradient_mode"))
            opt->value = enabled;
        else if (ConfigOptionInt *opt = preset_bundle->project_config.option<ConfigOptionInt>("mixed_filament_gradient_mode"))
            opt->value = enabled ? 1 : 0;
        else
            preset_bundle->project_config.set_key_value("mixed_filament_gradient_mode", new ConfigOptionBool(enabled));
    };
    auto notify_mixed_change = [print_cfg]() {
        if (!print_cfg)
            return;
        if (auto *print_tab = wxGetApp().get_tab(Preset::TYPE_PRINT))
            print_tab->update_dirty();
        if (wxGetApp().mainframe)
            wxGetApp().mainframe->on_config_changed(print_cfg);
    };
    auto decode_gradient_ids = [num_physical](const std::string &encoded) {
        std::vector<unsigned int> ids;
        if (encoded.empty() || num_physical == 0)
            return ids;
        bool seen[10] = { false };
        for (const char c : encoded) {
            if (c < '1' || c > '9')
                continue;
            const unsigned int id = unsigned(c - '0');
            if (id == 0 || id > num_physical || seen[id])
                continue;
            seen[id] = true;
            ids.emplace_back(id);
        }
        return ids;
    };
    auto encode_gradient_ids = [num_physical](const std::vector<unsigned int> &ids) {
        std::string encoded;
        bool seen[10] = { false };
        for (const unsigned int id : ids) {
            if (id == 0 || id > num_physical || id > 9 || seen[id])
                continue;
            seen[id] = true;
            encoded.push_back(char('0' + id));
        }
        return encoded;
    };
    auto decode_gradient_weights = [](const std::string &encoded, size_t expected_count) {
        std::vector<int> out;
        if (encoded.empty() || expected_count == 0)
            return out;
        std::string token;
        for (const char c : encoded) {
            if (c >= '0' && c <= '9') {
                token.push_back(c);
                continue;
            }
            if (!token.empty()) {
                out.emplace_back(std::max(0, std::atoi(token.c_str())));
                token.clear();
            }
        }
        if (!token.empty())
            out.emplace_back(std::max(0, std::atoi(token.c_str())));
        if (out.size() != expected_count)
            out.clear();
        return out;
    };
    auto normalize_gradient_weights = [](const std::vector<int> &weights, size_t n) {
        std::vector<int> out = weights;
        if (out.size() != n)
            out.assign(n, (n > 0) ? int(100 / n) : 0);
        int sum = 0;
        for (int &v : out) {
            v = std::max(0, v);
            sum += v;
        }
        if (sum <= 0 && n > 0) {
            out.assign(n, 0);
            out[0] = 100;
            return out;
        }
        std::vector<double> rem(n, 0.);
        int assigned = 0;
        for (size_t i = 0; i < n; ++i) {
            const double exact = 100.0 * double(out[i]) / double(sum);
            out[i] = int(std::floor(exact));
            rem[i] = exact - double(out[i]);
            assigned += out[i];
        }
        int missing = std::max(0, 100 - assigned);
        while (missing > 0) {
            size_t best_idx = 0;
            double best_rem = -1.0;
            for (size_t i = 0; i < rem.size(); ++i) {
                if (rem[i] > best_rem) {
                    best_rem = rem[i];
                    best_idx = i;
                }
            }
            ++out[best_idx];
            rem[best_idx] = 0.0;
            --missing;
        }
        return out;
    };
    auto encode_gradient_weights = [](const std::vector<int> &weights) {
        std::ostringstream ss;
        for (size_t i = 0; i < weights.size(); ++i) {
            if (i > 0)
                ss << '/';
            ss << std::max(0, weights[i]);
        }
        return ss.str();
    };
    auto build_weighted_multi_sequence = [normalize_gradient_weights](const std::vector<unsigned int> &ids,
                                                                      const std::vector<int> &weights,
                                                                      size_t max_cycle_limit) {
        if (ids.empty())
            return std::vector<unsigned int>();

        std::vector<unsigned int> filtered_ids;
        std::vector<int> counts;
        filtered_ids.reserve(ids.size());
        counts.reserve(ids.size());

        std::vector<int> normalized = normalize_gradient_weights(weights, ids.size());
        for (size_t i = 0; i < ids.size(); ++i) {
            const int weight = (i < normalized.size()) ? std::max(0, normalized[i]) : 0;
            if (weight <= 0)
                continue;
            filtered_ids.emplace_back(ids[i]);
            counts.emplace_back(weight);
        }
        if (filtered_ids.empty()) {
            filtered_ids = ids;
            counts.assign(ids.size(), 1);
        }

        int g = 0;
        for (const int c : counts)
            g = std::gcd(g, std::max(1, c));
        if (g > 1) {
            for (int &c : counts)
                c = std::max(1, c / g);
        }

        constexpr size_t k_max_cycle = 48;
        const size_t effective_cycle_limit =
            max_cycle_limit > 0 ? std::min(k_max_cycle, std::max<size_t>(1, max_cycle_limit)) : k_max_cycle;
        reduce_weight_counts_to_cycle_limit(counts, effective_cycle_limit);

        std::vector<unsigned int> reduced_ids;
        std::vector<int> reduced_counts;
        reduced_ids.reserve(filtered_ids.size());
        reduced_counts.reserve(counts.size());
        for (size_t i = 0; i < counts.size(); ++i) {
            if (counts[i] <= 0)
                continue;
            reduced_ids.emplace_back(filtered_ids[i]);
            reduced_counts.emplace_back(counts[i]);
        }
        if (reduced_ids.empty())
            return std::vector<unsigned int>();
        filtered_ids = std::move(reduced_ids);
        counts = std::move(reduced_counts);

        const int total = std::accumulate(counts.begin(), counts.end(), 0);
        if (total <= 0)
            return std::vector<unsigned int>(filtered_ids.begin(), filtered_ids.end());

        const size_t cycle = size_t(total);

        std::vector<unsigned int> sequence;
        sequence.reserve(cycle);
        std::vector<int> emitted(counts.size(), 0);
        for (size_t pos = 0; pos < cycle; ++pos) {
            size_t best_idx = 0;
            double best_score = -1e9;
            for (size_t i = 0; i < counts.size(); ++i) {
                const double target = double(pos + 1) * double(counts[i]) / double(total);
                const double score = target - double(emitted[i]);
                if (score > best_score) {
                    best_score = score;
                    best_idx = i;
                }
            }
            ++emitted[best_idx];
            sequence.emplace_back(filtered_ids[best_idx]);
        }
        if (sequence.empty())
            sequence = filtered_ids;
        return sequence;
    };
    auto decode_manual_pattern_ids = [num_physical](const std::string &pattern,
                                                    unsigned int       component_a,
                                                    unsigned int       component_b,
                                                    size_t             wall_loops) {
        return build_grouped_manual_pattern_preview_sequence(pattern, component_a, component_b, num_physical, wall_loops);
    };
    const bool height_weighted_mode = get_mixed_mode(false);
    int   gradient_mode = height_weighted_mode ? 1 : 0;
    float lower_bound   = std::max(0.01f, get_mixed_float("mixed_filament_height_lower_bound", 0.04f));
    float upper_bound   = std::max(lower_bound, get_mixed_float("mixed_filament_height_upper_bound", 0.16f));
    float preferred_local_z_a = std::max(0.f, get_mixed_float("mixed_color_layer_height_a", 0.f));
    float preferred_local_z_b = std::max(0.f, get_mixed_float("mixed_color_layer_height_b", 0.f));
    float nominal_layer_height = 0.2f;
    if (print_cfg && print_cfg->has("layer_height"))
        nominal_layer_height = float(print_cfg->opt_float("layer_height"));
    nominal_layer_height = std::max(0.01f, nominal_layer_height);
    size_t wall_loops = 1;
    if (print_cfg && print_cfg->has("wall_loops"))
        wall_loops = std::max<size_t>(1, size_t(std::max(1, print_cfg->opt_int("wall_loops"))));
    const bool local_z_mode = get_mixed_bool("dithering_local_z_mode", false);
    const bool component_bias_enabled = get_mixed_bool("mixed_filament_component_bias_enabled", false);
    float pointillism_pixel_size = std::max(0.f, get_mixed_float("mixed_filament_pointillism_pixel_size", 0.f));
    float pointillism_line_gap   = std::max(0.f, get_mixed_float("mixed_filament_pointillism_line_gap", 0.f));
    float mixed_surface_indentation = std::clamp(get_mixed_float("mixed_filament_surface_indentation", 0.f), -2.f, 2.f);
    bool  advanced_dithering = get_mixed_bool("mixed_filament_advanced_dithering", false);
    const std::string mixed_definitions = get_mixed_string("mixed_filament_definitions");
    const MixedFilamentPreviewSettings preview_settings {
        nominal_layer_height,
        lower_bound,
        upper_bound,
        preferred_local_z_a,
        preferred_local_z_b,
        local_z_mode,
        wall_loops
    };
    auto summarize_sequence = [num_physical](const std::vector<unsigned int> &sequence) {
        if (sequence.empty() || num_physical == 0)
            return std::string();
        std::vector<size_t> counts(num_physical + 1, size_t(0));
        size_t total = 0;
        for (const unsigned int id : sequence) {
            if (id == 0 || id > num_physical)
                continue;
            ++counts[id];
            ++total;
        }
        if (total == 0)
            return std::string();
        std::ostringstream ss;
        bool first = true;
        for (size_t id = 1; id <= num_physical; ++id) {
            if (counts[id] == 0)
                continue;
            const int pct = int(std::lround(100.0 * double(counts[id]) / double(total)));
            if (!first)
                ss << "  ";
            first = false;
            ss << "F" << id << ":" << pct << "%";
        }
        return ss.str();
    };
    auto blend_from_sequence = [num_physical](const std::vector<std::string> &colors, const std::vector<unsigned int> &sequence, const std::string &fallback) {
        if (colors.empty() || sequence.empty() || num_physical == 0)
            return fallback;
        std::vector<size_t> counts(num_physical + 1, size_t(0));
        size_t total = 0;
        for (const unsigned int id : sequence) {
            if (id == 0 || id > num_physical)
                continue;
            ++counts[id];
            ++total;
        }
        if (total == 0)
            return fallback;

        unsigned int first_id = 0;
        for (size_t id = 1; id <= num_physical; ++id) {
            if (counts[id] > 0) {
                first_id = unsigned(id);
                break;
            }
        }
        if (first_id == 0 || first_id > colors.size())
            return fallback;

        std::string blended = colors[first_id - 1];
        int         acc     = int(counts[first_id]);
        for (size_t id = size_t(first_id + 1); id <= num_physical; ++id) {
            if (counts[id] == 0 || id > colors.size())
                continue;
            blended = MixedFilamentManager::blend_color(blended, colors[id - 1], acc, int(counts[id]));
            acc += int(counts[id]);
        }
        return blended;
    };
    auto build_entry_preview_sequence = [decode_manual_pattern_ids, decode_gradient_ids, decode_gradient_weights,
                                         build_weighted_multi_sequence, preview_settings](const MixedFilament &entry) {
        const std::string normalized_pattern = MixedFilamentManager::normalize_manual_pattern(entry.manual_pattern);
        if (!normalized_pattern.empty())
            return decode_manual_pattern_ids(normalized_pattern,
                                             entry.component_a,
                                             entry.component_b,
                                             preview_settings.wall_loops);

        const bool simple_mode = entry.distribution_mode == int(MixedFilament::Simple);
        if (!simple_mode) {
            const std::vector<unsigned int> gradient_ids = decode_gradient_ids(entry.gradient_component_ids);
            if (gradient_ids.size() >= 3) {
                const std::vector<int> gradient_weights =
                    decode_gradient_weights(entry.gradient_component_weights, gradient_ids.size());
                return build_weighted_multi_sequence(gradient_ids, gradient_weights, 0);
            }
        }

        const int effective_mix_b = MixedFilamentConfigPanel::effective_local_z_preview_mix_b_percent(entry, preview_settings);
        const bool same_layer_mode = entry.distribution_mode == int(MixedFilament::SameLayerPointillisme);
        return build_effective_pair_preview_sequence(entry.component_a, entry.component_b, effective_mix_b, same_layer_mode);
    };
    auto compute_entry_display_color = [num_physical, &physical_colors, &nozzle_diameters, blend_from_sequence, build_entry_preview_sequence,
                                        preview_settings, component_bias_enabled](const MixedFilament &entry) {
        if (mixed_filament_supports_bias_apparent_color(entry, preview_settings, component_bias_enabled) &&
            entry.component_a >= 1 && entry.component_b >= 1 &&
            entry.component_a <= num_physical && entry.component_b <= num_physical &&
            entry.component_a <= physical_colors.size() && entry.component_b <= physical_colors.size()) {
            const auto [apparent_pct_a, apparent_pct_b] =
                mixed_filament_apparent_pair_percentages(entry, preview_settings, nozzle_diameters, component_bias_enabled);
            return MixedFilamentManager::blend_color(
                physical_colors[entry.component_a - 1],
                physical_colors[entry.component_b - 1],
                apparent_pct_a,
                apparent_pct_b);
        }

        const std::vector<unsigned int> sequence = build_entry_preview_sequence(entry);
        if (!sequence.empty())
            return blend_from_sequence(physical_colors, sequence, "#26A69A");

        if (entry.component_a == 0 || entry.component_b == 0 ||
            entry.component_a > num_physical || entry.component_b > num_physical ||
            entry.component_a > physical_colors.size() || entry.component_b > physical_colors.size()) {
            return std::string("#26A69A");
        }

        const int mix_b = std::clamp(entry.mix_b_percent, 0, 100);
        return MixedFilamentManager::blend_color(
            physical_colors[entry.component_a - 1],
            physical_colors[entry.component_b - 1],
            100 - mix_b,
            mix_b);
    };

    auto &mixed_mgr = preset_bundle->mixed_filaments;
    if (sync_manager) {
        mixed_mgr.auto_generate(physical_colors);
        mixed_mgr.clear_custom_entries();
        mixed_mgr.load_custom_entries(mixed_definitions, physical_colors);
        mixed_mgr.apply_gradient_settings(gradient_mode, lower_bound, upper_bound, advanced_dithering);
    }

    if (component_bias_enabled) {
        for (MixedFilament &entry : mixed_mgr.mixed_filaments()) {
            const float bias_value = mixed_filament_single_surface_offset_value(entry, nozzle_diameters);
            const auto balanced_pair = mixed_filament_single_surface_offset_pair(entry, bias_value, nozzle_diameters);
            entry.component_a_surface_offset = balanced_pair.first;
            entry.component_b_surface_offset = balanced_pair.second;
        }
    }

    // During project load, sidebar may refresh before physical filament combos
    // finish syncing. Avoid overwriting persisted mixed definitions while the
    // physical filament set is incomplete.
    if (num_physical >= 2) {
        set_mixed_mode(height_weighted_mode);
        set_mixed_bool("mixed_filament_component_bias_enabled", component_bias_enabled);
        set_mixed_float("mixed_filament_height_lower_bound", lower_bound);
        set_mixed_float("mixed_filament_height_upper_bound", upper_bound);
        set_mixed_float("mixed_color_layer_height_a", preferred_local_z_a);
        set_mixed_float("mixed_color_layer_height_b", preferred_local_z_b);
        set_mixed_float("mixed_filament_pointillism_pixel_size", pointillism_pixel_size);
        set_mixed_float("mixed_filament_pointillism_line_gap", pointillism_line_gap);
        set_mixed_float("mixed_filament_surface_indentation", mixed_surface_indentation);
        set_mixed_string("mixed_filament_definitions", mixed_mgr.serialize_custom_entries());
    }

    auto &mixed = mixed_mgr.mixed_filaments();
    const std::vector<size_t> ordered_mixed_indices = build_mixed_filament_ui_indices(mixed, p->m_mixed_filament_ui_order);
    std::vector<uint64_t>       sanitized_mixed_ui_order_ids;
    sanitized_mixed_ui_order_ids.reserve(ordered_mixed_indices.size());
    for (const size_t mixed_id : ordered_mixed_indices) {
        if (mixed_id < mixed.size() && mixed[mixed_id].stable_id != 0)
            sanitized_mixed_ui_order_ids.emplace_back(mixed[mixed_id].stable_id);
    }
    p->m_mixed_filament_ui_order = std::move(sanitized_mixed_ui_order_ids);

    p->m_mixed_filament_drag_active = false;
    p->m_mixed_filament_drag_source_mixed_id = size_t(-1);
    p->m_mixed_filament_row_bindings.clear();

    const int compact_gap_x   = FromDIP(6);
    const int compact_gap_y   = FromDIP(4);
    const int compact_row_pad = FromDIP(6);
    const bool is_dark = wxGetApp().dark_mode();
    const wxColour mixed_rows_bg = is_dark ? wxColour(45, 45, 49) : wxColour(246, 248, 251);
    const wxColour mixed_row_bg = is_dark ? wxColour(52, 52, 56) : wxColour(255, 255, 255);
    const wxColour mixed_row_hover_bg = is_dark ? wxColour(62, 62, 68) : wxColour(241, 247, 255);
    const wxColour mixed_text_fg = is_dark ? wxColour(232, 232, 232) : wxColour(20, 20, 20);
    const wxColour mixed_summary_fg = is_dark ? wxColour(182, 182, 182) : wxColour(96, 96, 96);
    p->m_panel_mixed_filaments_content->SetBackgroundColour(mixed_rows_bg);

    // Get the content sizer and clear it
    wxSizer *content_sizer = p->m_panel_mixed_filaments_content->GetSizer();
    if (content_sizer)
        content_sizer->Clear(true);
    
    // Re-add the top margin spacer that was added in constructor but cleared above
    if (content_sizer)
        content_sizer->AddSpacer(FromDIP(SidebarProps::ContentMargin()));

    // Update button states (buttons are now in title bar, created in constructor)
    if (p->m_btn_add_gradient)
        p->m_btn_add_gradient->Enable(num_physical >= 2);
    if (p->m_btn_add_pattern)
        p->m_btn_add_pattern->Enable(num_physical >= 2);
    if (p->m_btn_add_color)
        p->m_btn_add_color->Enable(num_physical >= 2);

    if (num_physical < 2) {
        p->m_panel_mixed_filaments_title->Hide();
        p->m_panel_mixed_filaments_content->Hide();
        Layout();
        refresh_model_canvas_colors();
        return;
    }

    // Show the panels
    p->m_panel_mixed_filaments_title->Show();
    p->m_panel_mixed_filaments_content->Show();
    
    // Reset the max size in case it was collapsed
    p->m_panel_mixed_filaments_content->SetMaxSize({-1, -1});

    auto *rows_scroller = new wxScrolledWindow(p->m_panel_mixed_filaments_content, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxTAB_TRAVERSAL);
    rows_scroller->SetScrollRate(0, FromDIP(6));
    rows_scroller->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_DEFAULT);
    rows_scroller->SetBackgroundColour(mixed_rows_bg);
    auto *rows_sizer = new wxBoxSizer(wxVERTICAL);
    rows_scroller->SetSizer(rows_sizer);

    if (mixed.empty()) {
        auto *empty_label = new wxStaticText(rows_scroller, wxID_ANY,
                                             _L("No mixed filaments yet. Use Add Gradient, Add Pattern, or Add Color to create one."));
        empty_label->SetForegroundColour(mixed_summary_fg);
        empty_label->SetFont(::Label::Body_13);
        empty_label->Wrap(FromDIP(360));
        rows_sizer->Add(empty_label, 0, wxALL | wxEXPAND, FromDIP(12));
        rows_scroller->Layout();
        rows_scroller->FitInside();
        const int empty_content_h = empty_label->GetBestSize().GetHeight() + FromDIP(28);
        const int empty_rows_h = std::max(FromDIP(86), empty_content_h);
        rows_scroller->SetMinSize(wxSize(-1, empty_rows_h));
        rows_scroller->SetMaxSize(wxSize(-1, empty_rows_h));
        if (content_sizer)
            content_sizer->Add(rows_scroller, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(SidebarProps::ContentMargin()));
        p->m_panel_mixed_filaments_content->Layout();
        Layout();
        refresh_model_canvas_colors();
        return;
    }

    auto adjust_rows_scroller_height = [this, rows_scroller]() {
        if (!rows_scroller)
            return;
        const int min_h = FromDIP(68);
        const int collapsed_max_h = FromDIP(220);
        int two_rows_cap_h = collapsed_max_h;
        const auto &children = rows_scroller->GetChildren();
        if (!children.empty()) {
            std::vector<int> heights;
            heights.reserve(children.GetCount());
            for (wxWindowList::compatibility_iterator it = children.GetFirst(); it; it = it->GetNext()) {
                wxWindow *child = it->GetData();
                wxPanel *panel = dynamic_cast<wxPanel *>(child);
                if (!panel)
                    continue;
                heights.emplace_back(std::max(panel->GetSize().GetHeight(), panel->GetBestSize().GetHeight()));
            }
            if (!heights.empty()) {
                std::sort(heights.begin(), heights.end(), std::greater<int>());
                const size_t keep = std::min<size_t>(2, heights.size());
                int rows_h = 0;
                for (size_t i = 0; i < keep; ++i)
                    rows_h += heights[i];
                if (keep > 1)
                    rows_h += int(keep - 1) * FromDIP(2);
                rows_h += FromDIP(8);
                two_rows_cap_h = std::max(collapsed_max_h, rows_h);
            }
        }

        const int max_h = p->m_expanded_mixed_filament_rows.empty() ? collapsed_max_h : two_rows_cap_h;
        const int content_h = std::max(0, rows_scroller->GetVirtualSize().GetHeight());
        const int desired_h = std::clamp(content_h, min_h, max_h);
        rows_scroller->SetMinSize(wxSize(-1, desired_h));
        rows_scroller->SetMaxSize(wxSize(-1, desired_h));
    };

    for (auto it = p->m_expanded_mixed_filament_rows.begin(); it != p->m_expanded_mixed_filament_rows.end();) {
        if (*it >= mixed.size() || mixed[*it].deleted)
            it = p->m_expanded_mixed_filament_rows.erase(it);
        else
            ++it;
    }

    std::vector<wxColour> palette;
    palette.reserve(physical_colors.size());
    for (const std::string &hex : physical_colors)
        palette.emplace_back(parse_mixed_color(hex));

    auto mixed_summary_text = [decode_gradient_ids](const MixedFilament &entry) {
        const std::string normalized_pattern = MixedFilamentManager::normalize_manual_pattern(entry.manual_pattern);
        if (!entry.custom)
            return wxString::Format("(Filament %u + Filament %u)", unsigned(entry.component_a), unsigned(entry.component_b));
        if (!normalized_pattern.empty())
            return _L("(Pattern)");
        if (decode_gradient_ids(entry.gradient_component_ids).size() >= 3)
            return _L("(Color)");
        return wxString::Format("(F%u + F%u)", unsigned(entry.component_a), unsigned(entry.component_b));
    };

    auto apply_mixed_entry_changes = [this, preset_bundle, print_cfg, num_physical](size_t mixed_id,
                                                                                    const MixedFilament &updated_mf,
                                                                                    bool preserve_enabled = false,
                                                                                    bool rebuild_virtual_id_remap = false) {
        if (!preset_bundle)
            return;

        auto &mgr = preset_bundle->mixed_filaments;
        auto &mfs = mgr.mixed_filaments();
        if (mixed_id >= mfs.size())
            return;

        const std::vector<MixedFilament> old_mixed = rebuild_virtual_id_remap ? mfs : std::vector<MixedFilament>();
        MixedFilament merged = updated_mf;
        if (preserve_enabled)
            merged.enabled = mfs[mixed_id].enabled;
        mfs[mixed_id] = merged;

        const std::string serialized = mgr.serialize_custom_entries();
        if (print_cfg) {
            if (ConfigOptionString *opt = print_cfg->option<ConfigOptionString>("mixed_filament_definitions"))
                opt->value = serialized;
            else
                print_cfg->set_key_value("mixed_filament_definitions", new ConfigOptionString(serialized));
        }
        if (ConfigOptionString *opt = preset_bundle->project_config.option<ConfigOptionString>("mixed_filament_definitions"))
            opt->value = serialized;
        else
            preset_bundle->project_config.set_key_value("mixed_filament_definitions", new ConfigOptionString(serialized));

        if (print_cfg) {
            if (auto *print_tab = wxGetApp().get_tab(Preset::TYPE_PRINT))
                print_tab->update_dirty();
            if (wxGetApp().mainframe)
                wxGetApp().mainframe->on_config_changed(print_cfg);
        }
        if (wxGetApp().plater())
            wxGetApp().plater()->update_project_dirty_from_presets();

        if (rebuild_virtual_id_remap)
            preset_bundle->update_mixed_filament_id_remap(old_mixed, num_physical, num_physical);

        int mode = 0;
        if (const ConfigOptionBool *opt = preset_bundle->project_config.option<ConfigOptionBool>("mixed_filament_gradient_mode"))
            mode = opt->value ? 1 : 0;
        else if (const ConfigOptionInt *opt = preset_bundle->project_config.option<ConfigOptionInt>("mixed_filament_gradient_mode"))
            mode = opt->value != 0 ? 1 : 0;
        float lo = preset_bundle->project_config.has("mixed_filament_height_lower_bound") ?
            float(preset_bundle->project_config.opt_float("mixed_filament_height_lower_bound")) : 0.04f;
        float hi = preset_bundle->project_config.has("mixed_filament_height_upper_bound") ?
            float(preset_bundle->project_config.opt_float("mixed_filament_height_upper_bound")) : 0.16f;
        bool advanced = false;
        if (const ConfigOptionBool *opt = preset_bundle->project_config.option<ConfigOptionBool>("mixed_filament_advanced_dithering"))
            advanced = opt->value;
        mode = std::clamp(mode, 0, 1);
        lo = std::max(0.01f, lo);
        hi = std::max(lo, hi);
        mgr.apply_gradient_settings(mode, lo, hi, advanced);
        update_dynamic_filament_list();

        if (rebuild_virtual_id_remap && wxGetApp().plater()) {
            p->m_skip_mixed_filament_sync_once = true;
            wxGetApp().plater()->on_filaments_change(num_physical);
        }
    };

    auto current_mixed_filament_ui_order = [this, &mixed]() {
        std::vector<uint64_t> ordered_ids;
        ordered_ids.reserve(p->m_mixed_filament_row_bindings.size());
        for (const auto &binding : p->m_mixed_filament_row_bindings) {
            if (binding.mixed_id < mixed.size() && mixed[binding.mixed_id].stable_id != 0)
                ordered_ids.emplace_back(mixed[binding.mixed_id].stable_id);
        }
        return ordered_ids;
    };

    auto drop_insert_position = [this]() {
        const wxPoint mouse_pos = wxGetMousePosition();
        size_t        visible_idx = 0;
        for (const auto &binding : p->m_mixed_filament_row_bindings) {
            if (binding.row == nullptr || !binding.row->IsShown())
                continue;

            const wxPoint top_left = binding.row->ClientToScreen(wxPoint(0, 0));
            const int     row_h = std::max(binding.row->GetSize().GetHeight(), binding.row->GetBestSize().GetHeight());
            const int     center_y = top_left.y + row_h / 2;
            if (mouse_pos.y < center_y)
                return visible_idx;

            ++visible_idx;
        }
        return visible_idx;
    };

    for (size_t display_mixed_idx = 0; display_mixed_idx < ordered_mixed_indices.size(); ++display_mixed_idx) {
        const size_t mixed_id = ordered_mixed_indices[display_mixed_idx];
        MixedFilament &mf = mixed[mixed_id];
        const bool auto_row = !mf.custom;

        auto *row = new wxPanel(rows_scroller, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        row->SetBackgroundColour(mixed_row_bg);
        auto *row_sizer = new wxBoxSizer(wxVERTICAL);
        p->m_mixed_filament_row_bindings.push_back({mixed_id, row});

        auto *header_panel = new wxPanel(row, wxID_ANY);
        header_panel->SetBackgroundColour(mixed_row_bg);
        auto *header_sizer = new wxBoxSizer(wxHORIZONTAL);

        const std::string synced_color = compute_entry_display_color(mf);
        if (mf.display_color != synced_color)
            mf.display_color = synced_color;
        auto *drag_handle = new MixedFilamentDragHandle(header_panel, mixed_summary_fg, mixed_row_bg);
        drag_handle->SetToolTip(_L("Drag to reorder mixed filaments in this panel."));
        header_sizer->Add(drag_handle, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, compact_gap_x);

        wxColour swatch_color = parse_mixed_color(mf.display_color);
        auto *swatch = new wxPanel(header_panel, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(12), FromDIP(12)));
        swatch->SetBackgroundColour(swatch_color);
        swatch->SetMinSize(wxSize(FromDIP(12), FromDIP(12)));
        header_sizer->Add(swatch, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, compact_gap_x);

        const int virtual_filament_id = int(num_physical + display_mixed_idx + 1);
        auto *name_label = new wxStaticText(header_panel, wxID_ANY, wxString::Format("Mixed Filament %d", virtual_filament_id));
        name_label->SetForegroundColour(mixed_text_fg);
        header_sizer->Add(name_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, compact_gap_x);

        auto *summary_label = new wxStaticText(header_panel, wxID_ANY, mixed_summary_text(mf));
        summary_label->SetForegroundColour(mixed_summary_fg);
        header_sizer->Add(summary_label, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, compact_gap_x);

        header_sizer->AddStretchSpacer(1);

        auto *enabled_chk = new wxCheckBox(header_panel, wxID_ANY, _L("Enabled"));
        enabled_chk->SetValue(mf.enabled);
        enabled_chk->SetForegroundColour(mixed_text_fg);
        header_sizer->Add(enabled_chk, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, compact_gap_x);
        enabled_chk->Bind(wxEVT_LEFT_UP, [](wxMouseEvent &evt) {
            evt.StopPropagation();
            evt.Skip();
        });
        enabled_chk->Bind(wxEVT_CHECKBOX, [mixed_id, enabled_chk, apply_mixed_entry_changes, preset_bundle](wxCommandEvent &) {
            if (!preset_bundle || !enabled_chk)
                return;
            auto &mgr = preset_bundle->mixed_filaments;
            auto &mfs = mgr.mixed_filaments();
            if (mixed_id >= mfs.size())
                return;
            MixedFilament updated = mfs[mixed_id];
            updated.enabled = enabled_chk->GetValue();
            apply_mixed_entry_changes(mixed_id, updated, false, true);
        });

        auto *del_btn = new ScalableButton(header_panel, wxID_ANY, "cross"); 
        del_btn->SetToolTip(_L("Delete mixed filament"));
        header_sizer->Add(del_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, compact_gap_x);
        
        del_btn->Bind(wxEVT_BUTTON, [this, mixed_id, num_physical, set_mixed_string, notify_mixed_change](wxCommandEvent&) {
             if (wxGetApp().preset_bundle) {
                 auto &mgr = wxGetApp().preset_bundle->mixed_filaments;
                 auto &mfs = mgr.mixed_filaments();
                 if (mixed_id < mfs.size()) {
                     const std::vector<MixedFilament> old_mixed = mfs;
                     auto canonical_pair = [](unsigned int a, unsigned int b) {
                         return std::make_pair(std::min(a, b), std::max(a, b));
                     };
                     MixedFilament &target = mfs[mixed_id];
                     const auto target_pair = canonical_pair(target.component_a, target.component_b);
                     const bool valid_auto_pair = target_pair.first >= 1 &&
                                                  target_pair.second >= 1 &&
                                                  target_pair.first <= num_physical &&
                                                  target_pair.second <= num_physical &&
                                                  target_pair.first != target_pair.second;
                     if (target.custom && target.origin_auto && valid_auto_pair) {
                         bool tombstoned_existing_auto = false;
                         for (size_t idx = 0; idx < mfs.size(); ++idx) {
                             if (idx == mixed_id)
                                 continue;
                             MixedFilament &candidate = mfs[idx];
                             if (candidate.custom)
                                 continue;
                             if (canonical_pair(candidate.component_a, candidate.component_b) != target_pair)
                                 continue;
                             candidate.deleted = true;
                             candidate.enabled = false;
                             tombstoned_existing_auto = true;
                             break;
                         }

                         if (tombstoned_existing_auto) {
                             mfs.erase(mfs.begin() + mixed_id);
                         } else {
                             target.component_a = target_pair.first;
                             target.component_b = target_pair.second;
                             target.mix_b_percent = 50;
                             target.ratio_a = 1;
                             target.ratio_b = 1;
                             target.manual_pattern.clear();
                             target.gradient_component_ids.clear();
                             target.gradient_component_weights.clear();
                             target.pointillism_all_filaments = false;
                             target.distribution_mode = int(MixedFilament::Simple);
                             target.custom = false;
                             target.origin_auto = true;
                             target.deleted = true;
                             target.enabled = false;
                         }
                     } else if (target.custom) {
                         mfs.erase(mfs.begin() + mixed_id);
                     } else {
                         target.deleted = true;
                         target.enabled = false;
                     }
                     p->m_expanded_mixed_filament_rows.clear();
                     set_mixed_string("mixed_filament_definitions", mgr.serialize_custom_entries());
                     wxGetApp().preset_bundle->update_mixed_filament_id_remap(old_mixed, num_physical, num_physical);
                     notify_mixed_change();
                     if (wxGetApp().plater())
                         wxGetApp().plater()->update_project_dirty_from_presets();
                     if (wxGetApp().plater()) {
                         p->m_skip_mixed_filament_sync_once = true;
                         wxGetApp().plater()->on_filaments_change(num_physical);
                     }
                 }
             }
        });

        header_panel->SetSizer(header_sizer);
        row_sizer->Add(header_panel, 0, wxEXPAND | wxALL, 0);

        auto *editor_host = new wxPanel(row, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        editor_host->SetBackgroundColour(mixed_row_bg);
        auto *editor_sizer = new wxBoxSizer(wxVERTICAL);
        editor_host->SetSizer(editor_sizer);
        editor_host->Hide();
        row_sizer->Add(editor_host, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, compact_row_pad);

        auto set_row_hover = [row, header_panel, editor_host, drag_handle, mixed_summary_fg, mixed_row_bg, mixed_row_hover_bg](bool hovered) {
            const wxColour bg = hovered ? mixed_row_hover_bg : mixed_row_bg;
            if (row) row->SetBackgroundColour(bg);
            if (header_panel) header_panel->SetBackgroundColour(bg);
            if (editor_host) editor_host->SetBackgroundColour(bg);
            if (drag_handle) drag_handle->set_colors(mixed_summary_fg, bg);
            if (row) row->Refresh();
            if (header_panel) header_panel->Refresh();
            if (editor_host) editor_host->Refresh();
        };

        auto row_contains_mouse = [row]() {
            if (!row)
                return false;
            const wxPoint mouse_pos = wxGetMousePosition();
            const wxPoint local = row->ScreenToClient(mouse_pos);
            return row->GetClientRect().Contains(local);
        };

        auto ensure_editor = [this, mixed_id, num_physical, physical_colors, nozzle_diameters, palette, preview_settings, component_bias_enabled, preset_bundle,
                              editor_host, editor_sizer, swatch, summary_label, header_panel, row,
                              rows_scroller, mixed_summary_text, apply_mixed_entry_changes]() {
            if (!preset_bundle || !editor_sizer || editor_sizer->GetItemCount() > 0)
                return;

            auto &mgr = preset_bundle->mixed_filaments;
            auto &mfs = mgr.mixed_filaments();
            if (mixed_id >= mfs.size())
                return;

            auto *editor = new MixedFilamentConfigPanel(editor_host, mixed_id, mfs[mixed_id], num_physical, physical_colors, nozzle_diameters, palette, preview_settings,
                component_bias_enabled,
                [this, mixed_id, swatch, summary_label, header_panel, row, rows_scroller, mixed_summary_text, apply_mixed_entry_changes](const MixedFilament &updated_mf) {
                    apply_mixed_entry_changes(mixed_id, updated_mf, true);

                    if (swatch) {
                        swatch->SetBackgroundColour(parse_mixed_color(updated_mf.display_color));
                        swatch->Refresh();
                    }
                    if (summary_label) {
                        summary_label->SetLabel(mixed_summary_text(updated_mf));
                    }
                    if (header_panel)
                        header_panel->Layout();
                    if (row)
                        row->Layout();
                    if (rows_scroller) {
                        rows_scroller->Layout();
                        rows_scroller->FitInside();
                    }
                });

            editor_sizer->Add(editor, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(4));
            editor_host->Layout();
        };

        auto toggle_editor = [this, mixed_id, editor_host, ensure_editor, rows_scroller, adjust_rows_scroller_height]() {
            if (!editor_host || !rows_scroller)
                return;

            if (editor_host->IsShown()) {
                editor_host->Hide();
                p->m_expanded_mixed_filament_rows.erase(mixed_id);
            } else {
                ensure_editor();
                editor_host->Show();
                p->m_expanded_mixed_filament_rows.insert(mixed_id);
            }

            rows_scroller->Layout();
            rows_scroller->FitInside();
            adjust_rows_scroller_height();
            p->m_panel_mixed_filaments_content->Layout();
            m_scrolled_sizer->Layout();
            Layout();
        };

        auto bind_toggle_target = [&toggle_editor](wxWindow *target) {
            if (!target)
                return;
            target->SetCursor(wxCursor(wxCURSOR_HAND));
            target->Bind(wxEVT_LEFT_UP, [toggle_editor](wxMouseEvent &) {
                toggle_editor();
            });
        };

        auto bind_hover_target = [set_row_hover, row_contains_mouse](wxWindow *target) {
            if (!target)
                return;
            target->Bind(wxEVT_ENTER_WINDOW, [set_row_hover](wxMouseEvent &evt) {
                set_row_hover(true);
                evt.Skip();
            });
            target->Bind(wxEVT_LEAVE_WINDOW, [set_row_hover, row_contains_mouse](wxMouseEvent &evt) {
                set_row_hover(row_contains_mouse());
                evt.Skip();
            });
        };

        auto release_drag_capture = [this]() {
            p->m_mixed_filament_drag_active = false;
            p->m_mixed_filament_drag_source_mixed_id = size_t(-1);
        };

        auto bind_drag_target = [this,
                                 mixed_id,
                                 &mixed,
                                 drop_insert_position,
                                 current_mixed_filament_ui_order,
                                 release_drag_capture](wxWindow *target) {
            if (!target)
                return;

            target->Bind(wxEVT_LEFT_DOWN, [this, mixed_id, target](wxMouseEvent &evt) {
                if (!target)
                    return;
                p->m_mixed_filament_drag_active = true;
                p->m_mixed_filament_drag_source_mixed_id = mixed_id;
                if (!target->HasCapture())
                    target->CaptureMouse();
                evt.StopPropagation();
            });

            target->Bind(wxEVT_MOTION, [this](wxMouseEvent &evt) {
                if (p->m_mixed_filament_drag_active)
                    evt.StopPropagation();
            });

            target->Bind(wxEVT_LEFT_UP, [this, &mixed, target, drop_insert_position, current_mixed_filament_ui_order](wxMouseEvent &evt) {
                if (target && target->HasCapture())
                    target->ReleaseMouse();

                if (!p->m_mixed_filament_drag_active || p->m_mixed_filament_drag_source_mixed_id >= mixed.size()) {
                    p->m_mixed_filament_drag_active = false;
                    p->m_mixed_filament_drag_source_mixed_id = size_t(-1);
                    evt.StopPropagation();
                    return;
                }

                const size_t source_mixed_id = p->m_mixed_filament_drag_source_mixed_id;
                p->m_mixed_filament_drag_active = false;
                p->m_mixed_filament_drag_source_mixed_id = size_t(-1);

                std::vector<size_t> current_mixed_ids;
                current_mixed_ids.reserve(p->m_mixed_filament_row_bindings.size());
                for (const auto &binding : p->m_mixed_filament_row_bindings) {
                    if (binding.mixed_id < mixed.size() && !mixed[binding.mixed_id].deleted)
                        current_mixed_ids.emplace_back(binding.mixed_id);
                }

                const auto source_it = std::find(current_mixed_ids.begin(), current_mixed_ids.end(), source_mixed_id);
                if (source_it == current_mixed_ids.end()) {
                    evt.StopPropagation();
                    return;
                }

                const size_t source_pos = size_t(std::distance(current_mixed_ids.begin(), source_it));
                size_t       insert_pos = drop_insert_position();
                insert_pos = std::min(insert_pos, current_mixed_ids.size());

                current_mixed_ids.erase(source_it);
                if (insert_pos > source_pos)
                    --insert_pos;
                insert_pos = std::min(insert_pos, current_mixed_ids.size());
                current_mixed_ids.insert(current_mixed_ids.begin() + ptrdiff_t(insert_pos), source_mixed_id);

                std::vector<uint64_t> reordered_stable_ids;
                reordered_stable_ids.reserve(current_mixed_ids.size());
                for (const size_t row_mixed_id : current_mixed_ids) {
                    if (row_mixed_id < mixed.size() && mixed[row_mixed_id].stable_id != 0)
                        reordered_stable_ids.emplace_back(mixed[row_mixed_id].stable_id);
                }

                if (reordered_stable_ids != current_mixed_filament_ui_order()) {
                    p->m_mixed_filament_ui_order = std::move(reordered_stable_ids);
                    update_mixed_filament_panel(false);
                }

                evt.StopPropagation();
            });

            target->Bind(wxEVT_MOUSE_CAPTURE_LOST, [release_drag_capture](wxMouseCaptureLostEvent &) {
                release_drag_capture();
            });
        };

        header_panel->SetToolTip(auto_row ?
            _L("Click to edit automatic mixed filament settings (saved as custom).") :
            _L("Click to expand/retract mixed filament settings"));
        bind_toggle_target(row);
        bind_toggle_target(header_panel);
        bind_toggle_target(name_label);
        bind_toggle_target(summary_label);
        bind_toggle_target(swatch);
        bind_hover_target(row);
        bind_hover_target(header_panel);
        bind_hover_target(name_label);
        bind_hover_target(summary_label);
        bind_hover_target(swatch);
        bind_hover_target(drag_handle);
        bind_drag_target(drag_handle);

        del_btn->Bind(wxEVT_LEFT_UP, [](wxMouseEvent &evt) {
            evt.StopPropagation();
            evt.Skip();
        });

        if (p->m_expanded_mixed_filament_rows.count(mixed_id) != 0) {
            ensure_editor();
            editor_host->Show();
        }

        row->SetSizer(row_sizer);
        rows_sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(2));
        rows_sizer->AddSpacer(FromDIP(2));
    }

    rows_sizer->AddSpacer(FromDIP(2));
    rows_scroller->Layout();
    rows_scroller->FitInside();
    adjust_rows_scroller_height();
    if (prev_rows_view_y > 0)
        rows_scroller->Scroll(0, prev_rows_view_y);

    content_sizer->Add(rows_scroller, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(2));
    content_sizer->AddSpacer(FromDIP(2));
    p->m_panel_mixed_filaments_content->Layout();
    m_scrolled_sizer->Layout();
    Layout();
    refresh_model_canvas_colors();
}

std::vector<unsigned int> Sidebar::get_ui_ordered_filament_ids() const
{
    const size_t num_physical = static_cast<size_t>(std::max(wxGetApp().filaments_cnt(), 0));
    std::vector<unsigned int> ordered_filament_ids;
    ordered_filament_ids.reserve(num_physical);
    for (size_t idx = 0; idx < num_physical; ++idx)
        ordered_filament_ids.emplace_back(unsigned(idx + 1));

    if (wxGetApp().preset_bundle == nullptr)
        return ordered_filament_ids;

    const auto &mixed = wxGetApp().preset_bundle->mixed_filaments.mixed_filaments();
    if (mixed.empty())
        return ordered_filament_ids;

    const std::vector<size_t> ordered_mixed_indices = build_mixed_filament_ui_indices(mixed, p->m_mixed_filament_ui_order);
    std::vector<unsigned int> actual_filament_id_by_mixed_idx(mixed.size(), 0);
    unsigned int              next_filament_id = unsigned(num_physical + 1);
    for (size_t mixed_idx = 0; mixed_idx < mixed.size(); ++mixed_idx) {
        if (!mixed[mixed_idx].enabled || mixed[mixed_idx].deleted)
            continue;
        actual_filament_id_by_mixed_idx[mixed_idx] = next_filament_id++;
    }

    ordered_filament_ids.reserve(size_t(next_filament_id - 1));
    for (const size_t mixed_idx : ordered_mixed_indices) {
        if (mixed_idx >= actual_filament_id_by_mixed_idx.size())
            continue;
        const unsigned int actual_filament_id = actual_filament_id_by_mixed_idx[mixed_idx];
        if (actual_filament_id != 0)
            ordered_filament_ids.emplace_back(actual_filament_id);
    }

    return ordered_filament_ids;
}
