#include <resona/module.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

extern "C" const resona_module *resona_module_get();

int main() {
    const auto &module = *resona_module_get();
    std::vector<float> input(65536), reference(input.size()), candidate(input.size());
    for (unsigned i = 0; i < input.size(); ++i)
        input[i] = 0.43f * std::sin(i * 0.058f) + 0.21f * std::sin(i * 0.149f);
    auto render = [&](auto &output, unsigned block, unsigned voice, float growl) {
        alignas(16) unsigned char state[4096]{};
        module.init(state, nullptr);
        module.set_param(state, 0, growl);
        module.set_param(state, 1, voice);
        for (unsigned pos = 0; pos < input.size();) {
            if (pos == 4096) module.set_param(state, 2, 1);
            if (pos == 24576) {
                module.set_param(state, 1, (voice + 1) % 4);
                module.set_param(state, 2, 0);
            }
            if (pos == 49152) module.set_param(state, 3, 0);
            const unsigned boundary = pos < 4096 ? 4096 : pos < 24576 ? 24576 : pos < 49152 ? 49152 : input.size();
            const float *in = input.data() + pos;
            float *out = output.data() + pos;
            const unsigned frames = std::min(block, boundary - pos);
            module.process(state, &in, &out, frames);
            pos += frames;
        }
    };
    float difference = 0, peak = 0, dryError = 0;
    for (unsigned voice = 0; voice < 4; ++voice) {
        for (float growl : {0.f, 64.f, 100.f}) {
            render(reference, 64, voice, growl);
            for (unsigned block : {1u, 17u, 63u}) {
                render(candidate, block, voice, growl);
                for (unsigned i = 0; i < candidate.size(); ++i) {
                    if (!std::isfinite(candidate[i])) return 2;
                    difference = std::max(difference, std::abs(candidate[i] - reference[i]));
                    peak = std::max(peak, std::abs(candidate[i]));
                    if (i >= 60000) dryError = std::max(dryError, std::abs(candidate[i] - input[i]));
                }
            }
        }
    }
    std::printf("maximum block-size difference %.9g, output peak %.6f, settled DRY error %.9g\n", difference, peak, dryError);
    return difference == 0 && peak <= 1 && dryError <= 1e-6f ? 0 : 1;
}
