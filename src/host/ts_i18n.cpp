#include "host/ts_i18n.hpp"

namespace ts::host {

namespace {

/// Null until the application installs one, which is the state every test binary runs in.
///
/// Release/acquire rather than relaxed: the resolver a caller reads through this pointer will touch
/// whatever table the installer built just before calling `set_translator`, and relaxed ordering
/// would permit a reader to see the pointer without seeing that table.
std::atomic<Translator> g_translator{nullptr};

} // namespace

void set_translator(Translator translator) noexcept
{
    g_translator.store(translator, std::memory_order_release);
}

const char* translate(const char* msgid) noexcept
{
    const Translator translator = g_translator.load(std::memory_order_acquire);

    // Untranslated is a working state, not a failure: this layer's English msgids are the strings
    // themselves, so a host that never installs a resolver still gets readable messages.
    return translator != nullptr ? translator(msgid) : msgid;
}

} // namespace ts::host
