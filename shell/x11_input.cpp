#include "x11_input.h"
#include <QByteArray>
#include <QList>
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <array>
#include <map>

struct X11Input::State {
    GdkDisplay *gdkDisplay = nullptr;
    Display *display = nullptr;
    Window root = 0;
    unsigned int numLock = 0;
    std::map<std::pair<unsigned int, unsigned int>, std::function<void()>> actions;

    QList<unsigned long> property(Window window, const char *name, Atom type) const
    {
        const auto atom = XInternAtom(display, name, True);
        if (!atom) return {};
        Atom actual = 0;
        int format = 0;
        unsigned long count = 0, remaining = 0;
        unsigned char *data = nullptr;
        gdk_x11_display_error_trap_push(gdkDisplay);
        const auto status = XGetWindowProperty(display, window, atom, 0, 128, False, type,
                                               &actual, &format, &count, &remaining, &data);
        const auto error = gdk_x11_display_error_trap_pop(gdkDisplay);
        QList<unsigned long> result;
        if (!error && status == Success && data && format == 32) {
            auto *values = reinterpret_cast<unsigned long *>(data);
            for (unsigned long i = 0; i < count; ++i) result.append(values[i]);
        }
        if (data) XFree(data);
        return result;
    }

    std::array<unsigned int, 4> variants(unsigned int mods) const
    {
        return {mods, mods | LockMask, mods | numLock, mods | LockMask | numLock};
    }

    static GdkFilterReturn filter(GdkXEvent *event, GdkEvent *, gpointer context)
    {
        auto &self = *static_cast<State *>(context);
        const auto &xevent = *static_cast<XEvent *>(event);
        if (xevent.type != KeyPress || xevent.xkey.window != self.root) return GDK_FILTER_CONTINUE;
        const auto mods = xevent.xkey.state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);
        const auto action = self.actions.find({xevent.xkey.keycode, mods});
        if (action == self.actions.end()) return GDK_FILTER_CONTINUE;
        action->second();
        return GDK_FILTER_REMOVE;
    }
};

X11Input::X11Input() : state_(std::make_unique<State>())
{
    auto &state = *state_;
    auto *display = gdk_display_get_default();
    if (!display || !GDK_IS_X11_DISPLAY(display)) return;
    state.gdkDisplay = display;
    state.display = gdk_x11_display_get_xdisplay(display);
    state.root = DefaultRootWindow(state.display);
    const auto code = XKeysymToKeycode(state.display, XK_Num_Lock);
    if (code) {
        if (auto *mods = XGetModifierMapping(state.display)) {
            for (auto mod = 0; mod < 8; ++mod)
                for (auto column = 0; column < mods->max_keypermod; ++column)
                    if (mods->modifiermap[mod * mods->max_keypermod + column] == code)
                        state.numLock |= 1u << mod;
            XFreeModifiermap(mods);
        }
    }
    gdk_window_add_filter(nullptr, State::filter, &state);
}

X11Input::~X11Input()
{
    if (!state_->display) return;
    gdk_window_remove_filter(nullptr, State::filter, state_.get());
    clear();
}

bool X11Input::add(QKeyCombination combination, std::function<void()> action)
{
    auto &state = *state_;
    if (!state.display) return false;
    const auto key = combination.key();
    KeySym symbol = NoSymbol;
    if (key >= Qt::Key_A && key <= Qt::Key_Z) symbol = XK_a + key - Qt::Key_A;
    else if (key >= Qt::Key_0 && key <= Qt::Key_9) symbol = XK_0 + key - Qt::Key_0;
    else if (key >= Qt::Key_F1 && key <= Qt::Key_F24) symbol = XK_F1 + key - Qt::Key_F1;
    else if (key == Qt::Key_Space) symbol = XK_space;
    if (!symbol) return false;
    const auto code = XKeysymToKeycode(state.display, symbol);
    if (!code) return false;
    const auto flags = combination.keyboardModifiers();
    const unsigned int mods = (flags.testFlag(Qt::ControlModifier) ? ControlMask : 0) |
                              (flags.testFlag(Qt::AltModifier) ? Mod1Mask : 0) |
                              (flags.testFlag(Qt::ShiftModifier) ? ShiftMask : 0) |
                              (flags.testFlag(Qt::MetaModifier) ? Mod4Mask : 0);
    if (state.actions.count({code, mods})) return false;
    gdk_x11_display_error_trap_push(state.gdkDisplay);
    for (const auto variant : state.variants(mods))
        XGrabKey(state.display, code, variant, state.root, False, GrabModeAsync, GrabModeAsync);
    if (gdk_x11_display_error_trap_pop(state.gdkDisplay)) {
        for (const auto variant : state.variants(mods)) XUngrabKey(state.display, code, variant, state.root);
        XFlush(state.display);
        return false;
    }
    state.actions.emplace(std::make_pair(code, mods), std::move(action));
    return true;
}

void X11Input::clear()
{
    auto &state = *state_;
    if (!state.display) return;
    for (const auto &[shortcut, callback] : state.actions)
        for (const auto variant : state.variants(shortcut.second))
            XUngrabKey(state.display, shortcut.first, variant, state.root);
    XFlush(state.display);
    state.actions.clear();
}

std::pair<int, QString> X11Input::layout() const
{
    auto &state = *state_;
    if (!state.display) return {};
    XkbStateRec keyboard{};
    if (XkbGetState(state.display, XkbUseCoreKbd, &keyboard) != Success) return {};
    const auto atom = XInternAtom(state.display, "_XKB_RULES_NAMES", True);
    if (!atom) return {};
    Atom actual = 0;
    int format = 0;
    unsigned long count = 0, remaining = 0;
    unsigned char *data = nullptr;
    const auto status = XGetWindowProperty(state.display, state.root, atom, 0, 1024, False, XA_STRING,
                                           &actual, &format, &count, &remaining, &data);
    if (status != Success || !data) return {};
    const auto fields = QByteArray(reinterpret_cast<const char *>(data), count).split('\0');
    XFree(data);
    const auto label = QString::fromLatin1(fields.value(2).split(',').value(keyboard.group)).toUpper();
    if (label == "RU") return {1049, label};
    if (label == "US" || label == "GB") return {1033, "EN"};
    return label.isEmpty() ? std::pair<int, QString>{} : std::make_pair(2000 + int(keyboard.group), label);
}

X11Foreground X11Input::foreground() const
{
    const auto &state = *state_;
    if (!state.display) return {};
    const auto window = state.property(state.root, "_NET_ACTIVE_WINDOW", XA_WINDOW).value(0);
    if (!window) return {};
    const auto pid = state.property(window, "_NET_WM_PID", XA_CARDINAL).value(0);
    const auto flags = state.property(window, "_NET_WM_STATE", XA_ATOM);
    const auto fullscreen = XInternAtom(state.display, "_NET_WM_STATE_FULLSCREEN", True);
    return {window, pid, fullscreen && flags.contains(fullscreen)};
}

bool X11Input::activate(unsigned long window) const
{
    const auto &state = *state_;
    if (!state.display || !window) return false;
    const auto atom = XInternAtom(state.display, "_NET_ACTIVE_WINDOW", True);
    if (!atom) return false;
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = atom;
    event.xclient.format = 32;
    event.xclient.data.l[0] = 1;
    event.xclient.data.l[1] = CurrentTime;
    const auto sent = XSendEvent(state.display, state.root, False,
                                SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(state.display);
    return sent != 0;
}

bool X11Input::pasteShortcutHeld() const
{
    const auto &state = *state_;
    if (!state.display) return false;
    char keys[32]{};
    XQueryKeymap(state.display, keys);
    const KeySym shortcut[] = {XK_Control_L, XK_Control_R, XK_Alt_L, XK_Alt_R, XK_Shift_L, XK_Shift_R, XK_v};
    for (const auto symbol : shortcut) {
        const auto code = XKeysymToKeycode(state.display, symbol);
        if (code && (keys[code / 8] & (1 << (code % 8)))) return true;
    }
    return false;
}

bool X11Input::paste(unsigned long window) const
{
    const auto &state = *state_;
    if (!state.display || foreground().window != window) return false;
    int event = 0, error = 0, major = 0, minor = 0;
    if (!XTestQueryExtension(state.display, &event, &error, &major, &minor)) return false;
    const auto control = XKeysymToKeycode(state.display, XK_Control_L);
    const auto v = XKeysymToKeycode(state.display, XK_v);
    if (!control || !v) return false;
    XTestFakeKeyEvent(state.display, control, True, CurrentTime);
    XTestFakeKeyEvent(state.display, v, True, CurrentTime);
    XTestFakeKeyEvent(state.display, v, False, CurrentTime);
    XTestFakeKeyEvent(state.display, control, False, CurrentTime);
    XFlush(state.display);
    return true;
}
