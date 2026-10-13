// Packages a recorded dataset session, as the runtime's upload queue does
// before encrypting and uploading it (docs/dataset-format.md, "Package").
//
//   dataset_package <session dir> [--qp N]
//
// Writes <session dir>/package.tar and prints the package block. Uses the
// hardware H.265 encoder, so it runs on the headset only.
#include "capture/packager.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: dataset_package <session dir> [--qp N]\n");
        return 2;
    }
    babble::capture::PackagerOptions o;
    for (int i = 2; i + 1 < argc; i += 2)
        if (strcmp(argv[i], "--qp") == 0)
            o.qp = atoi(argv[i + 1]);
    int last = -1;
    o.progress = [&](double p) {
        if (int(p * 20) != last) {
            last = int(p * 20);
            fprintf(stderr, "\r%3d%%", last * 5);
        }
    };
    const auto r = babble::capture::make_package(argv[1], o);
    fprintf(stderr, "\n");
    if (!r.ok) {
        fprintf(stderr, "dataset_package: %s%s\n", r.error.c_str(), r.retry ? " (would be retried)" : "");
        return 1;
    }
    printf("%s\n%s\n", r.tar.c_str(), r.package.dump(2).c_str());
    return 0;
}
