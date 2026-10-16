#include "capture/capture_protocol.h"

namespace babble::capture {

using json = nlohmann::json;

json Connection::error(const json &id, const std::string &code, const std::string &message) {
    json e = {{"op", "error"}, {"code", code}, {"message", message}};
    if (!id.is_null())
        e["id"] = id;
    return e;
}

Connection::Output Connection::handle_line(const std::string &line, Backend &backend) {
    Output out;
    const json req = json::parse(line, nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        out.messages.push_back(error(nullptr, "bad_request", "not a JSON object"));
        return out;
    }
    const json id = req.contains("id") ? req["id"] : json(nullptr);
    auto reply = [&](json j) {
        if (!id.is_null())
            j["id"] = id;
        out.messages.push_back(std::move(j));
    };
    auto fail = [&](const std::string &code, const std::string &message) {
        out.messages.push_back(error(id, code, message));
    };
    auto from = [&](const Result &r, json ok) {
        if (!r.ok)
            return fail(r.code, r.message);
        ok.update(r.data);
        reply(std::move(ok));
    };
    if (!req.contains("op") || !req["op"].is_string())
        return fail("bad_request", "missing \"op\""), out;
    const std::string op = req["op"];

    if (op == "hello") {
        const int v = req.value("protocol", 0);
        if (v != kProtocol) {
            fail("protocol", "protocol " + std::to_string(v) + " is not supported; this runtime speaks " +
                                 std::to_string(kProtocol));
            out.close = true;
            return out;
        }
        greeted_ = true;
        json h = {{"op", "hello"}, {"protocol", kProtocol}};
        h.update(backend.hello_info());
        reply(std::move(h));
        return out;
    }
    if (!greeted_) {
        fail("protocol", "send hello first");
        out.close = true;
        return out;
    }
    if (op == "start") {
        if ((req.contains("routine") && !req["routine"].is_string()) ||
            (req.contains("meta") && !req["meta"].is_object()) ||
            (req.contains("consent") && !req["consent"].is_object() && !req["consent"].is_null()) ||
            (req.contains("routine_def") && !req["routine_def"].is_object() && !req["routine_def"].is_null()))
            return fail("bad_request", "start: routine is a string; meta, consent and routine_def are objects"), out;
        from(backend.start(req), {{"op", "started"}});
    } else if (op == "segment") {
        if (!req.contains("label") || !req["label"].is_string() || !req.contains("start_ns") ||
            !req["start_ns"].is_number_unsigned() || !req.contains("end_ns") || !req["end_ns"].is_number_unsigned())
            return fail("bad_request", "segment needs label, start_ns and end_ns (CLOCK_MONOTONIC ns)"), out;
        if ((req.contains("step") && !req["step"].is_number_integer()) ||
            (req.contains("phase") && !req["phase"].is_string()))
            return fail("bad_request", "segment: step is an integer, phase a string"), out;
        Segment seg{req["label"], req["start_ns"], req["end_ns"], std::nullopt, req.value("phase", "")};
        if (req.contains("step"))
            seg.step = req["step"].get<int64_t>();
        const Result r = backend.segment(seg);
        if (!r.ok)
            fail(r.code, r.message);
        else if (!id.is_null())
            reply({{"op", "segment_ok"}});
    } else if (op == "stop") {
        const Result r = backend.stop(kStoppedByApp);
        if (!r.ok)
            return fail(r.code, r.message), out;
        stop_id_ = id.is_null() ? json(true) : id;   // The "stopped" event follows when the session is finished.
    } else if (op == "status") {
        json s = backend.status();
        s["op"] = "status";
        reply(std::move(s));
    } else if (op == "upload") {
        if (!req.contains("session") || !req["session"].is_string())
            return fail("bad_request", "upload needs a session id"), out;
        from(backend.queue_upload(req["session"]), {{"op", "upload_queued"}, {"session", req["session"]}});
    } else {
        fail("bad_request", "unknown op \"" + op + "\"");
    }
    return out;
}

}
