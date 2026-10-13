// Runs an eye model over recorded frames, preprocessed exactly as BabbleEye
// does it (view 3 flipped vertically, INTER_AREA resize to the model's R,
// (x/255 - 0.5)/0.25, batched [2,1,R,R]). For comparing model outputs across
// codecs and settings on the same footage.
//
//   ffmpeg -i eye.mkv -f rawvideo -pix_fmt gray - | eye_replay <model.pte> <width> <height> > out.f32
//
// Input: raw gray frames on stdin, left|right side by side (the recorder's
// stereo_lr layout), width x height each. Output: every output element of
// the model per frame, as little-endian float32 on stdout.
#include "model/executorch_runner.h"
#include "model/half.h"
#include "model/image_ops.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace babble;

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: eye_replay <model.pte> <width> <height>  (raw gray L|R frames on stdin)\n");
        return 2;
    }
    const uint32_t w = uint32_t(atoi(argv[2])), h = uint32_t(atoi(argv[3])), eye_w = w / 2;
    ExecuTorchRunner::set_threads(1);
    ExecuTorchRunner m;
    if (!m.load(argv[1]))
        return 1;
    const TensorSpec &is = m.inputs()[0], &os = m.outputs()[0];
    const uint32_t res = uint32_t(is.shape.back());
    const size_t plane = size_t(res) * res;
    if (is.elements() != 2 * plane) {
        fprintf(stderr, "eye_replay: expected a [2,1,R,R] input\n");
        return 1;
    }

    std::vector<uint8_t> frame(size_t(w) * h), eye(size_t(eye_w) * h), small(plane);
    std::vector<float> x(2 * plane), y(os.elements());
    std::vector<uint16_t> half_in(is.elements()), half_out(os.elements());
    size_t frames = 0;
    while (fread(frame.data(), 1, frame.size(), stdin) == frame.size()) {
        for (int v = 0; v < 2; v++) {
            img::View src{frame.data() + v * eye_w, eye_w, h, w};
            if (v == 1) {   // "R" pane: rot90(2) then mirror == vertical flip
                img::flip_vertical(src, {eye.data(), eye_w, h, eye_w});
                src = {eye.data(), eye_w, h, eye_w};
            }
            img::resize_area(src, {small.data(), res, res, res});
            for (size_t i = 0; i < plane; i++)
                x[v * plane + i] = (float(small[i]) / 255.0f - 0.5f) / 0.25f;
        }
        const void *in = x.data();
        if (is.dtype == DType::F16) {
            to_half(x.data(), half_in.data(), x.size());
            in = half_in.data();
        }
        void *out = os.dtype == DType::F16 ? static_cast<void *>(half_out.data()) : y.data();
        std::vector<TensorView> iv{{const_cast<void *>(in), &is}};
        std::vector<TensorView> ov{{out, &os}};
        if (!m.run(iv, ov))
            return 1;
        if (os.dtype == DType::F16)
            from_half(half_out.data(), y.data(), y.size());
        fwrite(y.data(), sizeof(float), y.size(), stdout);
        frames++;
    }
    fprintf(stderr, "eye_replay: %zu frames, %zu outputs each, R %u\n", frames, y.size(), res);
    return 0;
}
