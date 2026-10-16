#include "platform/media_graph.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>

#include <fcntl.h>
#include <linux/media.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace babble {

RouteCheck check_sensor_routes(const std::string &media_dev, const std::vector<std::string> &sensor_prefixes) {
    RouteCheck out;
    int fd = open(media_dev.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        out.error = media_dev + ": " + strerror(errno);
        return out;
    }

    // Get the counts, then the arrays, and try again if the graph changed between the two calls.
    std::vector<media_v2_entity> entities;
    std::vector<media_v2_pad> pads;
    std::vector<media_v2_link> links;
    bool got = false;
    for (int attempt = 0; attempt < 3 && !got; attempt++) {
        media_v2_topology topo{};
        if (ioctl(fd, MEDIA_IOC_G_TOPOLOGY, &topo) != 0)
            break;
        uint64_t version = topo.topology_version;
        entities.resize(topo.num_entities);
        pads.resize(topo.num_pads);
        links.resize(topo.num_links);
        topo.ptr_entities = uintptr_t(entities.data());
        topo.ptr_pads = uintptr_t(pads.data());
        topo.ptr_links = uintptr_t(links.data());
        topo.ptr_interfaces = 0;
        if (ioctl(fd, MEDIA_IOC_G_TOPOLOGY, &topo) != 0)
            break;
        got = topo.topology_version == version && topo.num_entities == entities.size() &&
              topo.num_pads == pads.size() && topo.num_links == links.size();
    }
    int err = errno;
    close(fd);
    if (!got) {
        out.error = std::string("MEDIA_IOC_G_TOPOLOGY failed: ") + strerror(err);
        return out;
    }

    std::map<uint32_t, uint32_t> pad_entity;
    for (const auto &p : pads)
        pad_entity[p.id] = p.entity_id;
    std::map<uint32_t, const media_v2_entity *> by_id;
    for (const auto &e : entities)
        by_id[e.id] = &e;
    // This map holds the enabled data links from each entity to its downstream entities.
    std::multimap<uint32_t, uint32_t> next;
    for (const auto &l : links) {
        if ((l.flags & MEDIA_LNK_FL_LINK_TYPE) != MEDIA_LNK_FL_DATA_LINK || !(l.flags & MEDIA_LNK_FL_ENABLED))
            continue;
        auto s = pad_entity.find(l.source_id), d = pad_entity.find(l.sink_id);
        if (s != pad_entity.end() && d != pad_entity.end())
            next.emplace(s->second, d->second);
    }

    out.ok = true;
    for (const auto &prefix : sensor_prefixes) {
        const media_v2_entity *sensor = nullptr;
        for (const auto &e : entities)
            if (strncmp(e.name, prefix.c_str(), prefix.size()) == 0)
                sensor = &e;
        bool routed = false;
        if (sensor) {
            // Walk breadth-first along the enabled links until the walk finds a capture I/O node.
            std::set<uint32_t> seen{sensor->id};
            std::vector<uint32_t> frontier{sensor->id};
            while (!frontier.empty() && !routed) {
                std::vector<uint32_t> nf;
                for (uint32_t id : frontier) {
                    auto [b, e] = next.equal_range(id);
                    for (auto it = b; it != e; ++it) {
                        auto ent = by_id.find(it->second);
                        if (ent != by_id.end() && ent->second->function == MEDIA_ENT_F_IO_V4L)
                            routed = true;
                        if (seen.insert(it->second).second)
                            nf.push_back(it->second);
                    }
                }
                frontier.swap(nf);
            }
        }
        if (!routed) {
            out.ok = false;
            out.unrouted.push_back(prefix);
        }
    }
    return out;
}

}
