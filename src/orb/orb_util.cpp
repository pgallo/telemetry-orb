#include "orb/orb_util.hpp"

#include <omniORB4/Naming.hh>

#include <cerrno>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace telemetry::orb {

void write_ior_file(CORBA::ORB_ptr orb, CORBA::Object_ptr obj, const std::string& path) {
    CORBA::String_var ior = orb->object_to_string(obj);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + tmp);
        out << ior.in() << '\n';
    }
    std::filesystem::rename(tmp, path);
}

CORBA::Object_ptr read_ior_file(CORBA::ORB_ptr orb, const std::string& path,
                                std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!std::filesystem::exists(path)) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("timed out waiting for " + path);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::ifstream in(path);
    std::string ior;
    in >> ior;
    if (ior.empty()) throw std::runtime_error("empty IOR file " + path);
    return orb->string_to_object(ior.c_str());
}

namespace {

CosNaming::Name to_name(const std::string& text) {
    std::vector<std::string> parts;
    std::stringstream ss(text);
    for (std::string part; std::getline(ss, part, '/');)
        if (!part.empty()) parts.push_back(part);
    if (parts.empty()) throw std::invalid_argument("empty name");
    CosNaming::Name name;
    name.length(static_cast<CORBA::ULong>(parts.size()));
    for (CORBA::ULong i = 0; i < name.length(); ++i) {
        const auto dot = parts[i].rfind('.');
        name[i].id = parts[i].substr(0, dot).c_str();
        name[i].kind = dot == std::string::npos ? "" : parts[i].substr(dot + 1).c_str();
    }
    return name;
}

CosNaming::NamingContext_ptr root_context(CORBA::ORB_ptr orb) {
    CORBA::Object_var obj = orb->resolve_initial_references("NameService");
    CosNaming::NamingContext_var ctx = CosNaming::NamingContext::_narrow(obj);
    if (CORBA::is_nil(ctx)) throw std::runtime_error("NameService is not a naming context");
    return ctx._retn();
}

}  // namespace

void bind_name(CORBA::ORB_ptr orb, const std::string& text, CORBA::Object_ptr obj) {
    CosNaming::NamingContext_var ctx = root_context(orb);
    const CosNaming::Name full = to_name(text);
    for (CORBA::ULong i = 0; i + 1 < full.length(); ++i) {
        CosNaming::Name one;
        one.length(1);
        one[0] = full[i];
        try {
            CORBA::Object_var sub = ctx->resolve(one);
            ctx = CosNaming::NamingContext::_narrow(sub);
        } catch (const CosNaming::NamingContext::NotFound&) {
            ctx = ctx->bind_new_context(one);
        }
    }
    CosNaming::Name leaf;
    leaf.length(1);
    leaf[0] = full[full.length() - 1];
    ctx->rebind(leaf, obj);
}

CORBA::Object_ptr resolve_name(CORBA::ORB_ptr orb, const std::string& text) {
    CosNaming::NamingContext_var ctx = root_context(orb);
    return ctx->resolve(to_name(text));
}

sigset_t block_termination_signals() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    return set;
}

int wait_for_termination(const sigset_t& set, std::chrono::milliseconds timeout) {
    if (timeout.count() <= 0) {
        int sig = 0;
        sigwait(&set, &sig);
        return sig;
    }
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    ts.tv_nsec = static_cast<long>((timeout.count() % 1000) * 1'000'000);
    for (;;) {
        const int sig = sigtimedwait(&set, nullptr, &ts);
        if (sig > 0) return sig;
        if (errno == EAGAIN) return 0;
        if (errno != EINTR) return 0;
    }
}

std::string describe(const CORBA::Exception& ex) {
    std::string s = ex._name();
    if (auto* sys = CORBA::SystemException::_downcast(&ex))
        s += " (minor " + std::to_string(sys->minor()) + ")";
    return s;
}

}  // namespace telemetry::orb
