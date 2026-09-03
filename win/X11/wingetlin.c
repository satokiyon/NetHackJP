/* Modified by NetHackJP contributor @satokiyon; latest change date: 2026-09-03. */
/* NetHackJP: XIM-aware getlin / askname dialog.
 *
 * Phase 3+ of the XIM implementation plan: Xaw's AsciiText widget
 * with `XtNinternational=True` is unreliable on XIM clients (the
 * fcitx5 server does not engage on this widget in the WSL/XWayland
 * setup).  This file implements a small custom dialog that drives the
 * XIM protocol directly via the winxim.c API.
 *
 * The visual structure mirrors CreateDialog() in dialogs.c:
 *
 *     +---------------------------------+
 *     | <prompt>                        |
 *     | <response>                      |   <-- label, not editable
 *     |       [ OK ]   [ Cancel ]       |
 *     +---------------------------------+
 *
 * Keys are routed to xim_getlin_key_handler() which appends UTF-8
 * bytes from XIM to an internal buffer and updates the <response>
 * label after every event.
 *
 * The public API matches dialogs.c's signature so X11_askname() and
 * X11_getlin() in winX.c can switch over with a single-line change
 * (CreateDialog -> CreateXimDialog, GetDialogResponse ->
 * XimDialogGetResponse).
 */

#ifndef SYSV
#define PRESERVE_NO_SYSV /* X11 include files may define SYSV */
#endif

/* X11 headers must come BEFORE winX.h because winX.h uses Widget,
 * Dimension, Pixel, Boolean, XEvent, String, XtAppContext, etc. */
#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <X11/keysym.h> /* XK_Return, XK_Escape, XK_BackSpace, ... */
#include <X11/Xaw/Cardinals.h> /* ZERO / ONE macros for Xt{Set,Get}Values */
#include <X11/Xaw/Form.h>
#include <X11/Xaw/Label.h>
#include <X11/Xaw/Command.h>
#include <X11/Xlib.h> /* XK_*, XK_Return, etc. */

/* X11_BUILD must be defined before including hack.h so that GCC 12+'s
 * attribute-nonstring warning is suppressed for the IM-related
 * variadic-format calls we make.  See tradstdc.h for the exact check. */
#define X11_BUILD
#include "hack.h"
#undef X11_BUILD

#include "winX.h"

#include <string.h>

#ifdef PRESERVE_NO_SYSV
#ifdef SYSV
#undef SYSV
#endif
#undef PRESERVE_NO_SYSV
#endif

/* Maximum UTF-8 input length.  We use BUFSZ-1 to match the rest of
 * NetHackJP's input buffer size assumptions in winX.c. */
#define XIM_GETLIN_MAX 1023

/* ---- module state ---------------------------------------------------- */

/* Single global state since only one getlin dialog is active at a
 * time.  The form widget reference is needed so the OK / Cancel
 * callbacks can popdown the popup shell. */
typedef struct {
    char buf[XIM_GETLIN_MAX + 1];
    int len;
    Widget form;
    Widget prompt_label;
    Widget response_label;
    Widget ok_widget;        /* OK button, used by MapNotify focus handler */
    void *ic;                /* XIC for the form, created lazily */
    XtCallbackProc ok_cb;    /* user-provided OK callback (e.g. done_button) */
    XtCallbackProc cancel_cb; /* user-provided cancel callback (e.g. abort_button) */
} XimGetlinState;

static XimGetlinState g_xim_getlin_state;

/* ---- UTF-8 helpers --------------------------------------------------- */

/* Append up to n bytes from src to buf at *len, capped at max.
 * The cap is checked strictly so that *len + 1 <= max (the trailing
 * NUL is always present).  Bell on overflow. */
static void
utf8_append(char *buf, int *len, int max, const char *src, int n)
{
    int available = max - *len;

    if (n > available) {
        n = available;
        X11_nhbell();
    }
    if (n <= 0) {
        X11_nhbell();
        return;
    }
    (void) memcpy(buf + *len, src, (size_t) n);
    *len += n;
    buf[*len] = '\0';
}

/* Delete the last UTF-8 character from buf and update *len.  Walks
 * back past any UTF-8 continuation bytes (10xxxxxx). */
static void
utf8_backspace(char *buf, int *len)
{
    int i;

    if (*len <= 0)
        return;
    i = *len - 1;
    while (i > 0 && ((unsigned char) buf[i] & 0xC0U) == 0x80U)
        i--;
    *len = i;
    buf[i] = '\0';
}

/* ---- label update ---------------------------------------------------- */

/* Push the current buffer back into the response label so the user
 * sees what they have typed.  Because the label was wrapped with
 * X11_wrap_widget_if_Xft(), it does NOT respond to ordinary
 * XtSetValues on XtNlabel alone; we have to:
 *   1. Set XtNlabel via XtSetValues.
 *   2. Call X11_update_label_if_Xft() which reads the new XtNlabel,
 *      renders the Xft Pixmap and assigns XtNbitmap.
 *   3. Force an X exposure on the widget's window with XClearArea
 *      so the X server actually repaints the new bitmap. */
static void
xim_getlin_refresh_label(XimGetlinState *state)
{
    Arg args[1];
    Widget w = state->response_label;

    if (w == (Widget) 0)
        return;

    XtSetArg(args[0], XtNlabel, state->buf);
    XtSetValues(w, args, ONE);
    X11_update_label_if_Xft(w);

    if (XtIsRealized(w) && XtWindow(w) != None) {
        XClearArea(XtDisplay(w), XtWindow(w), 0, 0, 0, 0, True);
    }
}

/* ---- button callbacks ------------------------------------------------ */

/* Forward to the user-supplied OK callback (e.g. done_button or
 * askname_done) which is responsible for copying the final text into
 * the caller's buffer, popping the dialog down, and setting
 * exit_x_event.  We only need to forward the call. */
static void
xim_getlin_ok_button(Widget w, XtPointer client_data, XtPointer call_data)
{
    XimGetlinState *state = &g_xim_getlin_state;
    Widget form = (Widget) client_data;

    nhUse(form);
    nhUse(call_data);

    if (state->ok_cb != (XtCallbackProc) 0)
        (*state->ok_cb)(w, client_data, call_data);
}

/* Forward to the user-supplied cancel callback (abort_button).  We
 * also pre-populate the buffer with a single ESC so callers that read
 * the buffer directly (instead of trusting the cancel callback) see
 * the standard cancellation signal. */
static void
xim_getlin_cancel_button(Widget w, XtPointer client_data, XtPointer call_data)
{
    XimGetlinState *state = &g_xim_getlin_state;
    Widget form = (Widget) client_data;

    nhUse(form);
    nhUse(call_data);

    state->buf[0] = '\033';
    state->buf[1] = '\0';
    state->len = 1;
    xim_getlin_refresh_label(state);

    if (state->cancel_cb != (XtCallbackProc) 0)
        (*state->cancel_cb)(w, client_data, call_data);
}

/* ---- key event handler ---------------------------------------------- */

/*ARGSUSED*/
static void
xim_getlin_key_handler(Widget w, XtPointer client_data, XEvent *event,
                       Boolean *continue_to_dispatch)
{
    XimGetlinState *state = (XimGetlinState *) client_data;
    XKeyEvent *key = (XKeyEvent *) event;
    char buf[MAX_KEY_STRING];
    KeySym keysym = NoSymbol;
    int n;

    nhUse(w);
    nhUse(continue_to_dispatch);

    /* XLookupString always runs first because we need to detect
    // non-printable keys (Return, Escape, BackSpace).  XIM does not
    // see these keys (they are consumed by the X server's auto-repeat
    // and modifier logic). */
    n = XLookupString(key, buf, (int) sizeof buf - 1, &keysym,
                      (XComposeStatus *) 0);
    if (n >= 0 && n < (int) sizeof buf)
        buf[n] = '\0';

    /* Return / KP_Enter: handled by the OK button's own translation
     * (<Key>Return: set() notify() unset()).  We must NOT call
     * xim_getlin_ok_button() here, otherwise the user's OK callback
     * fires twice.  Just let Xt's translation dispatch take over. */
    if (keysym == XK_Return || keysym == XK_KP_Enter) {
        return;
    }
    /* Escape: cancel via our own handler.  The Cancel button's own
     * translation has been stripped so Escape does not activate it. */
    if (keysym == XK_Escape) {
        xim_getlin_cancel_button(w, client_data, (XtPointer) 0);
        return;
    }
    if (keysym == XK_BackSpace || keysym == XK_Delete) {
        utf8_backspace(state->buf, &state->len);
        xim_getlin_refresh_label(state);
        return;
    }

    /* Try XIM (fcitx5 / ibus).  When XIM is active, the committed UTF-8
     * string arrives as one chunk per IM commit event.  We attach the IC
     * to the OK button (the focused widget) rather than the form, so
     * fcitx5's XSelectInput registration on the IC's XNFocusWindow
     * actually receives the keyboard events that the X server delivers
     * to OK.  Attaching to the form ancestor does NOT work because the
     * X server routes events to the focused widget, not its parent. */
    if (state->ic == (void *) 0 && xim_is_active()) {
        state->ic = xim_create_ic((void *) state->ok_widget);
        if (state->ic != (void *) 0)
            xim_focus_in(state->ic);
    }
    if (state->ic != (void *) 0 && xim_is_active()) {
        char utf8buf[MAX_KEY_STRING];
        int xim_n;

        xim_n = xim_lookup_utf8(state->ic, key, utf8buf,
                                (int) sizeof utf8buf - 1,
                                (unsigned long *) 0, (int *) 0);
        if (xim_n > 0) {
            utf8_append(state->buf, &state->len, XIM_GETLIN_MAX,
                        utf8buf, xim_n);
            xim_getlin_refresh_label(state);
            return;
        }
        /* When XIM is active, the IM has already consumed or processed
         * the key.  Do NOT also fall through to the ASCII fallback
         * (which would insert a stray space character whenever the user
         * presses Ctrl+Space to toggle the IM, etc.). */
        return;
    }

    /* ASCII fallback for when XIM is not active.  This keeps the
     * dialog usable when fcitx5 / ibus is not running. */
    if (n >= 1 && buf[0] >= ' ' && (unsigned char) buf[0] < 0x7fU) {
        utf8_append(state->buf, &state->len, XIM_GETLIN_MAX, buf, 1);
        xim_getlin_refresh_label(state);
    }
}

/* ---- public API ------------------------------------------------------ */

/*
 * Create an XIM-aware dialog widget and return the form widget handle
 * (matching the convention of CreateDialog() in dialogs.c).
 *
 * The OK and Cancel buttons are managed by this module.  When the user
 * clicks them, the corresponding user-supplied callback is invoked
 * with the form widget as client_data; that callback is expected to
 * copy the current text (via XimDialogGetResponse()) into the
 * caller's buffer and set exit_x_event = TRUE.
 */
Widget
CreateXimDialog(Widget parent, String name,
                XtCallbackProc okay_callback,
                XtCallbackProc cancel_callback)
{
    Widget form, prompt, response, ok, cancel;
    Arg args[20];
    Cardinal num_args;

    /* Reset state */
    (void) memset(&g_xim_getlin_state, 0, sizeof g_xim_getlin_state);
    g_xim_getlin_state.ok_cb = okay_callback;
    g_xim_getlin_state.cancel_cb = cancel_callback;

    num_args = 0;
    XtSetArg(args[num_args], nhStr(XtNborderWidth), 0); num_args++;
    /* XtNdefaultDistance adds vertical/horizontal spacing between
     * children, so the prompt/response/OK buttons don't overlap.
     * 8 pixels gives a comfortable, readable spacing. */
    XtSetArg(args[num_args], nhStr(XtNdefaultDistance), 8); num_args++;
    /* NetHackJP: also set the form's preferred width so it does not
     * collapse to a narrow column when every child is chain-pinned
     * to XtChainLeft.  Without this, Xaw's Form widget sometimes
     * squeezes the response to a 1-character-wide vertical strip. */
    XtSetArg(args[num_args], nhStr(XtNwidth), 380); num_args++;
    form = XtCreateManagedWidget(name, formWidgetClass, parent,
                                 args, num_args);
    g_xim_getlin_state.form = form;

    /* prompt: top of the form, no chain hints.  We use explicit
     * XtNwidth so the form sizes around it. */
    num_args = 0;
    XtSetArg(args[num_args], nhStr(XtNborderWidth), 0); num_args++;
    XtSetArg(args[num_args], nhStr(XtNinternalWidth), 4); num_args++;
    XtSetArg(args[num_args], nhStr(XtNinternalHeight), 4); num_args++;
    XtSetArg(args[num_args], nhStr(XtNwidth), 360); num_args++;
    XtSetArg(args[num_args], nhStr(XtNheight), 28); num_args++;
    prompt = XtCreateManagedWidget("prompt", labelWidgetClass, form,
                                   args, num_args);
    g_xim_getlin_state.prompt_label = prompt;
    X11_wrap_widget_if_Xft(prompt, NHW_MENU);

    /* response: directly below the prompt.  We force an explicit
     * width AND height so the form does NOT allocate a 0-height
     * widget for the empty input.  XtNborderWidth is set to 1 so the
     * response area looks like an input field with a visible frame.
     * No chain hints: with both XtNtop/XtNbottom pinned to top the
     * Form widget interprets them as a fixed top region, leaving no
     * room for explicit XtNheight.  Removing them lets XtNheight
     * take effect. */
    num_args = 0;
    XtSetArg(args[num_args], nhStr(XtNfromVert), prompt); num_args++;
    XtSetArg(args[num_args], nhStr(XtNborderWidth), 1); num_args++;
    XtSetArg(args[num_args], nhStr(XtNinternalWidth), 4); num_args++;
    XtSetArg(args[num_args], nhStr(XtNinternalHeight), 4); num_args++;
    XtSetArg(args[num_args], nhStr(XtNwidth), 360); num_args++;
    XtSetArg(args[num_args], nhStr(XtNheight), 36); num_args++;
    XtSetArg(args[num_args], nhStr(XtNlabel), ""); num_args++;
    response = XtCreateManagedWidget("response", labelWidgetClass, form,
                                     args, num_args);
    g_xim_getlin_state.response_label = response;
    X11_wrap_widget_if_Xft(response, NHW_MENU);

    /* OK: below the response, on the left.  No chain hints, just
     * explicit dimensions.  The widget name "OK" (capitalized) matches
     * NetHack.ad resource patterns (NetHack*OK.foreground: green,
     * NetHack*OK.shapeStyle: roundedRectangle). */
    num_args = 0;
    XtSetArg(args[num_args], nhStr(XtNfromVert), response); num_args++;
    XtSetArg(args[num_args], nhStr(XtNborderWidth), 0); num_args++;
    /* Force a usable button size so the shapeStyle resource is visible.
     * The NetHack.ad shapeStyle resources render a rounded rectangle
     * border; without these dimensions the button is just text-sized. */
    XtSetArg(args[num_args], nhStr(XtNwidth), 100); num_args++;
    XtSetArg(args[num_args], nhStr(XtNheight), 32); num_args++;
    ok = XtCreateManagedWidget("OK", commandWidgetClass, form,
                               args, num_args);
    XtAddCallback(ok, XtNcallback, xim_getlin_ok_button,
                  (XtPointer) form);

    /*
     * Override the OK button's default translations so that the
     * Command widget does NOT activate itself on <Key>space.  Space
     * is reserved for fcitx5's commit, and we route it through our
     * XIM-aware key handler instead.  <Key>Return stays bound so
     * pressing Enter commits via the existing ok callback.
     *
     * #override replaces the widget's translations completely.
     * The <Btn1Down>...<Btn1Up> sequence is preserved so that mouse
     * clicks still activate the button.
     */
    XtOverrideTranslations(ok, XtParseTranslationTable(
        "#override\n\
         <Key>Return: set() notify() unset()\n\
         <Btn1Down>,<Btn1Up>: set() notify() unset()"));

    if (cancel_callback != (XtCallbackProc) 0) {
        num_args = 0;
        XtSetArg(args[num_args], nhStr(XtNfromVert), response); num_args++;
        XtSetArg(args[num_args], nhStr(XtNfromHoriz), ok); num_args++;
        XtSetArg(args[num_args], nhStr(XtNborderWidth), 0); num_args++;
        XtSetArg(args[num_args], nhStr(XtNwidth), 100); num_args++;
        XtSetArg(args[num_args], nhStr(XtNheight), 32); num_args++;
        cancel = XtCreateManagedWidget("cancel", commandWidgetClass, form,
                                       args, num_args);
        XtAddCallback(cancel, XtNcallback, xim_getlin_cancel_button,
                      (XtPointer) form);

        /* Cancel: remove all key bindings so Enter and Space do
         * not activate Cancel accidentally.  Only mouse clicks and
         * explicit focus traversal reach it. */
        XtOverrideTranslations(cancel, XtParseTranslationTable(
            "#override\n\
             <Btn1Down>,<Btn1Up>: set() notify() unset()"));
    }

    /*
     * Install the key event handler on the OK button.  The OK button
     * is focusable and will be the keyboard focus target below, so
     * it reliably receives KeyPress events.  Installing on the form
     * itself would not work because Xt dispatches key events to the
     * focused widget, not to its parent.
     *
     * nonmaskable = True forces the handler to be invoked for
     * KeyPress events even if the OK button's X event mask does not
     * include KeyPressMask (Command widgets select KeyPressMask by
     * default because of their default translations, but we have
     * overridden those and want to be sure the handler still fires).
     */
    XtAddEventHandler(ok, KeyPressMask, True,
                      (XtEventHandler) xim_getlin_key_handler,
                      (XtPointer) &g_xim_getlin_state);

    /*
     * Make the OK button the keyboard focus.  XtSetKeyboardFocus
     * takes a shell widget (or composite) as the first argument; the
     * popup shell we were passed qualifies.  We do this explicitly
     * (rather than relying on appResources.autofocus) because the
     * dialog is unusable without keyboard focus on a focusable child.
     */
    XtSetKeyboardFocus(parent, ok);

    return form;
}

/*
 * After the dialog has been realized and popped up, register a
 * StructureNotify handler on the popup shell that will move the X
 * server focus to the OK button *as soon as the popup is mapped*.
 *
 * Calling XSetInputFocus() directly here is unsafe: right after
 * XtPopup() returns, the form widget and its OK child may not yet
 * have been mapped (mapping happens asynchronously), and X refuses
 * to set focus on a window that is not viewable with BadMatch.
 * Waiting for the MapNotify event guarantees the OK window is
 * viewable by the time we call XSetInputFocus().
 */
static void
xim_map_notify_focus_handler(Widget w, XtPointer client_data,
                             XEvent *event, Boolean *cont)
{
    Widget ok;
    XimGetlinState *state = &g_xim_getlin_state;

    nhUse(client_data);
    nhUse(cont);

    if (event->type != MapNotify || w == (Widget) 0)
        return;

    ok = state->ok_widget;
    if (ok == (Widget) 0 || !XtIsRealized(ok))
        return;

    XSetInputFocus(XtDisplay(ok), XtWindow(ok),
                   RevertToParent, CurrentTime);

    /* Eagerly create the XIM IC on the OK button (the focused
     * widget) so that fcitx5 has already associated the IC with the
     * focused widget BEFORE the user starts typing.  Attaching the
     * IC to the OK window is critical: the X server delivers
     * key events to the OK window (not to its parent form), so
     * fcitx5's XSelectInput on the IC's XNFocusWindow must be on
     * the OK window for fcitx5 to receive them. */
    if (state->ic == (void *) 0 && xim_is_active()) {
        state->ic = xim_create_ic((void *) state->ok_widget);
        if (state->ic != (void *) 0)
            xim_focus_in(state->ic);
    }

    /* One-shot: we only need to focus the OK button once when the
     * popup is first mapped.  Remove this handler so it doesn't
     * fire on subsequent expose/visibility events. */
    XtRemoveEventHandler(w, StructureNotifyMask, False,
                         xim_map_notify_focus_handler, client_data);
}

void
XimDialogFocusInput(Widget form)
{
    Widget ok, popup;

    if (form == (Widget) 0)
        return;
    /* NetHackJP: the OK button is named "OK" (capitalized) to match
     * the NetHack.ad resource patterns ("NetHack*OK.foreground: green",
     * "NetHack*OK.shapeStyle: roundedRectangle", etc.).  Looking it up
     * with the WRONG case ("ok") would silently fail and leave the
     * dialog with no keyboard focus and no event handler handlers,
     * making the dialog look completely unresponsive. */
    ok = XtNameToWidget(form, "OK");
    if (ok == (Widget) 0)
        return;

    popup = XtParent(form);
    g_xim_getlin_state.ok_widget = ok;

    /* XtSetKeyboardFocus is safe at any time: if the widget is not
     * yet realized, Xt just records the focus intent and applies
     * it when XtSetKeyboardFocus is called again after realization. */
    XtSetKeyboardFocus(popup, ok);

    /* Install a one-shot MapNotify handler so that XSetInputFocus is
     * invoked exactly when the popup shell becomes viewable. */
    XtAddEventHandler(popup, StructureNotifyMask, False,
                      (XtEventHandler) xim_map_notify_focus_handler,
                      (XtPointer) NULL);
}

/*
 * Set the prompt label text.
 */
void
XimDialogSetPrompt(Widget w, String prompt)
{
    Arg args[1];

    nhUse(w);
    /* Cast away const for the XtSetArg macro; we are not modifying
     * the string, just passing it to Xt for storage. */
    if (prompt == (String) 0)
        prompt = (String) "";
    XtSetArg(args[0], XtNlabel, prompt);
    XtSetValues(g_xim_getlin_state.prompt_label, args, ONE);
    X11_update_label_if_Xft(g_xim_getlin_state.prompt_label);
}

/*
 * Pre-populate the input buffer with the given text.  Used to set the
 * default answer (e.g. current player name for #name).  Truncated to
 * XIM_GETLIN_MAX bytes.
 */
void
XimDialogSetResponse(Widget w, String text)
{
    int n;

    nhUse(w);
    /* Cast away const for memcpy; we are not modifying the source. */
    if (text == (String) 0)
        text = (String) "";
    n = (int) strlen(text);
    if (n > XIM_GETLIN_MAX)
        n = XIM_GETLIN_MAX;
    (void) memcpy(g_xim_getlin_state.buf, text, (size_t) n);
    g_xim_getlin_state.len = n;
    g_xim_getlin_state.buf[n] = '\0';
    xim_getlin_refresh_label(&g_xim_getlin_state);
}

/*
 * Return a freshly-allocated copy of the current buffer.  Caller is
 * responsible for XtFree()-ing the returned string.  Matches the
 * semantics of GetDialogResponse() in dialogs.c.
 */
String
XimDialogGetResponse(Widget w)
{
    nhUse(w);
    return XtNewString(g_xim_getlin_state.buf);
}

/* End wingetlin.c */