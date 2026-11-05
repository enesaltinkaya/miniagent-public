#include "ui/draw.h"

#include "ui/fonts.h"
#include "ui/rich_text.h"
#include "ui/view_rows.h"

// The row-model helpers this renderer shares with the TUI live in
// view_rows; TrimMessage was a static here before the extraction.
using viewrows::TrimMessage;

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <string>

#include "util/imagetex.h"

ImVec4 ColorForKind(App::UiMessage::Kind kind) {
    switch (kind) {
        case App::UiMessage::User:
            return ImVec4(0.45f, 0.7f, 1.0f, 1.0f);
        case App::UiMessage::Agent:
            return ImVec4(0.55f, 0.9f, 0.6f, 1.0f);
        case App::UiMessage::Thinking:
            return ImVec4(0.52f, 0.53f, 0.56f, 1.0f);
        case App::UiMessage::Tool:
            return ImVec4(0.95f, 0.85f, 0.4f, 1.0f);
        case App::UiMessage::Error:
            return ImVec4(1.0f, 0.45f, 0.4f, 1.0f);
        case App::UiMessage::System:
            return ImVec4(0.62f, 0.63f, 0.72f, 1.0f);
        case App::UiMessage::Warning:
            return ImVec4(0.95f, 0.72f, 0.35f, 1.0f);
    }
    return ImVec4(1, 1, 1, 1);
}

const char* LabelForKind(App::UiMessage::Kind kind) {
    switch (kind) {
        case App::UiMessage::User:
            return "";  // the blue bubble marks user messages
        case App::UiMessage::Agent:
            return "";  // mono text alone marks agent messages
        case App::UiMessage::Thinking:
            return "Thinking";
        case App::UiMessage::Tool:
            return "Tool";
        case App::UiMessage::Error:
            return "Error";
        case App::UiMessage::System:
            return "System";
        case App::UiMessage::Warning:
            return "Warning";
    }
    return "";
}

float PromptBoxHeight(const char* buf) {
    int lines = 1;
    for (const char* p = buf; *p; ++p)
        if (*p == '\n')
            ++lines;
    // The prompt box renders in the mono font, and this is also called from
    // the main layout reservation outside any font scope, so measure with
    // the mono font explicitly.
    ImGui::PushFont(gFontMono ? gFontMono : gFontDefault);
    float lineH = ImGui::GetTextLineHeightWithSpacing();
    ImGui::PopFont();
    float framePad = ImGui::GetStyle().FramePadding.y;
    float boxH = std::max((float)lines, 5.0f) * lineH + framePad * 2 + 2;
    boxH = std::min(boxH, 10.0f * lineH + framePad * 2 + 2);  // cap at 10 lines
    return boxH;
}

float InputBarHeight(const char* buf) {
    float boxH = PromptBoxHeight(buf);
    float padY = ImGui::GetStyle().WindowPadding.y;
    // The toggle row renders in the mono font that DrawInputBar pushes; its
    // frame height comes from the app-wide FramePadding default (same value
    // the toggle buttons render with).
    ImGui::PushFont(gFontMono ? gFontMono : gFontDefault);
    float toggleH = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
    ImGui::PopFont();
    return boxH + padY * 2 + 2 + toggleH;
}

float StatusStripHeight() {
    // The strip hosts a square default-padded button (the scaffold task-list
    // toggle) that is taller than the text line: size the band for whichever
    // is higher.
    return std::max(ImGui::GetTextLineHeightWithSpacing(), ImGui::GetFrameHeight()) +
           ImGui::GetStyle().WindowPadding.y * 2 + 2;
}

// ---- Message body selection --------------------------------------------
// Left-drag over a message's rich-text body selects a range of it; the
// selection is copied to the clipboard when the button is released (no
// extra Ctrl+C step). The geometry comes from the RichTextLayout that
// RenderRichText records for every message it draws; only the drawn band
// has a fresh layout, so off-viewport messages cannot be selected.

static int CharByteLen(const char* p) {
    unsigned char c = (unsigned char)p[0];
    if (c < 0x80)
        return 1;
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 1;
}

// The body a message's layout describes: Tool rows render their expanded
// result, every other kind renders text (Agent/Thinking in a trimmed
// view, see richTextOffset).
static const std::string& SelectableText(const App::UiMessage& m) {
    return m.kind == App::UiMessage::Tool ? m.toolResult : m.text;
}

static bool LayoutFresh(const App::UiMessage& m) {
    return !m.richLayout.lines.empty() && m.richLayout.frame == ImGui::GetFrameCount();
}

static float WordWidth(const RichTextWord& w, const char* base) {
    const char* p = base + w.byteStart;
    return w.font->CalcTextSizeA(w.fontSize, FLT_MAX, 0.0f, p, p + w.byteLen).x;
}

// Byte offset (within the message body) under `pos`, or -1 when the
// layout is stale or the point is not over a drawn line.
static int RichTextHit(const App::UiMessage& m, ImVec2 pos) {
    if (!LayoutFresh(m))
        return -1;
    const RichTextLayout& L = m.richLayout;
    const char* base = SelectableText(m).c_str() + m.richTextOffset;
    // Last visual line whose top is at or above the mouse; the recorded
    // lines are contiguous (``` toggles are the only unrecorded band).
    int li = -1;
    for (int i = 0; i < (int)L.lines.size(); ++i) {
        if (L.lines[i].y0 <= pos.y)
            li = i;
        else
            break;
    }
    if (li < 0)
        return -1;
    const RichTextLine& line = L.lines[li];
    if (pos.y >= line.y1)
        return -1;
    if (line.firstWord < 0)
        return line.byteStart;  // wordless (empty) line: anchor at its start
    const RichTextWord& first = L.words[line.firstWord];
    const RichTextWord& last = L.words[line.lastWord];
    if (pos.x < first.x)
        return line.byteStart;
    if (pos.x >= last.x + WordWidth(last, base))
        return line.byteEnd;
    for (int wi = line.firstWord; wi <= line.lastWord; ++wi) {
        const RichTextWord& w = L.words[wi];
        if (pos.x < w.x + WordWidth(w, base)) {
            // Inside this word: walk its characters, midpoint rule.
            const char* p = base + w.byteStart;
            const char* end = p + w.byteLen;
            float cx = w.x;
            while (p < end) {
                int clen = CharByteLen(p);
                float cw = w.font->CalcTextSizeA(w.fontSize, FLT_MAX, 0.0f, p, p + clen).x;
                if (pos.x < cx + cw * 0.5f)
                    return (int)(p - base);
                cx += cw;
                p += clen;
            }
            // In the trailing space after the word: the next word's start.
        }
    }
    return line.byteEnd;
}

// Blue overlay for the byte range [start, end) of the message body, drawn
// on top of the text (the message is re-rendered, so the layout and the
// highlight are rebuilt together every frame).
static void HighlightMessageBody(const App::UiMessage& m, ImDrawList* dl, int start, int end) {
    if (start > end)
        std::swap(start, end);
    if (start == end)
        return;
    const RichTextLayout& L = m.richLayout;
    if (L.frame != ImGui::GetFrameCount() || L.lines.empty())
        return;
    const char* base = SelectableText(m).c_str() + m.richTextOffset;
    const ImU32 col = IM_COL32(90, 140, 230, 96);
    for (const RichTextLine& line : L.lines) {
        if (line.firstWord < 0) {
            // Wordless line: a dot if its anchor byte is inside the range.
            if (start <= line.byteStart && line.byteStart < end)
                dl->AddRectFilled(ImVec2(line.x0, line.y0), ImVec2(line.x0 + 2.0f, line.y1), col);
            continue;
        }
        if (line.byteEnd <= start || line.byteStart >= end)
            continue;
        for (int wi = line.firstWord; wi <= line.lastWord; ++wi) {
            const RichTextWord& w = L.words[wi];
            if (w.byteStart + w.byteLen <= start || w.byteStart >= end)
                continue;
            float left = w.x;
            float right = w.x + WordWidth(w, base);
            if (w.byteStart < start) {
                const char* p = base + w.byteStart;
                const char* lim = base + start;
                float cx = w.x;
                while (p < lim) {
                    int clen = CharByteLen(p);
                    cx += w.font->CalcTextSizeA(w.fontSize, FLT_MAX, 0.0f, p, p + clen).x;
                    p += clen;
                }
                left = cx;
            }
            if (w.byteStart + w.byteLen > end) {
                const char* p = base + w.byteStart;
                const char* lim = base + end;
                float cx = w.x;
                while (p < lim) {
                    int clen = CharByteLen(p);
                    cx += w.font->CalcTextSizeA(w.fontSize, FLT_MAX, 0.0f, p, p + clen).x;
                    p += clen;
                }
                right = cx;
            }
            if (right < left)
                right = left + 2.0f;
            dl->AddRectFilled(ImVec2(left, line.y0), ImVec2(right, line.y1), col);
        }
    }
}

static void DrawSelectionHighlight(App& app) {
    App::MessageSelection& sel = app.msgSel;
    if (!sel.active || sel.msg >= app.uiMessages.size())
        return;
    HighlightMessageBody(app.uiMessages[sel.msg], ImGui::GetWindowDrawList(), sel.start, sel.end);
}

// Starts / tracks / finalizes the in-progress body selection. Called at
// the end of DrawMessageLog, after the floating Sessions button, so that
// IsItemHovered() below refers to that button and a click on it cannot
// also start a selection.
static void UpdateMessageSelection(App& app) {
    App::MessageSelection& sel = app.msgSel;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool anyModal = app.sessionsModalOpen || app.treeOpen || app.scaffoldTasksOpen;
    const bool sessionsHovered = ImGui::IsItemHovered();
    // The floating scaffold task panel is drawn over this log (after the
    // buttons above): its clicks belong to it, not to the message text it
    // covers.
    const bool panelHovered = MouseOverScaffoldTasksPanel();

    if (sel.active) {
        const bool stale = sel.msg >= app.uiMessages.size();
        if (stale) {
            sel.active = false;  // the message list was rebuilt underneath
        } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Released: copy the selection (if any) and drop it.
            App::UiMessage& m = app.uiMessages[sel.msg];
            const std::string& text = SelectableText(m);
            int s = std::min(sel.start, sel.end);
            int e = std::max(sel.start, sel.end);
            const int len = (int)text.size() - m.richTextOffset;
            s = std::max(0, std::min(s, len));
            e = std::max(0, std::min(e, len));
            if (e > s) {
                app.setClipboardText(
                    std::string(text.c_str() + m.richTextOffset + s, e - s));
                app.showToast("Text copied");
            }
            sel.active = false;
        } else {
            // Dragging: track the mouse, clamped to the selected message.
            App::UiMessage& m = app.uiMessages[sel.msg];
            const int len = (int)SelectableText(m).size() - m.richTextOffset;
            int hit = RichTextHit(m, mouse);
            if (hit >= 0) {
                sel.end = hit;
            } else if (LayoutFresh(m)) {
                // The mouse left the drawn body: clamp to the nearest end.
                const RichTextLine& top = m.richLayout.lines.front();
                const RichTextLine& bot = m.richLayout.lines.back();
                if (mouse.y < top.y0)
                    sel.end = 0;
                else if (mouse.y > bot.y1)
                    sel.end = len;
            }
            sel.start = std::max(0, std::min(sel.start, len));
            sel.end = std::max(0, std::min(sel.end, len));
        }
    } else if (!anyModal && !sessionsHovered && !panelHovered &&
               ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        for (size_t i = 0; i < app.uiMessages.size(); ++i) {
            int hit = RichTextHit(app.uiMessages[i], mouse);
            if (hit >= 0) {
                sel.active = true;
                sel.msg = i;
                sel.start = sel.end = hit;
                break;
            }
        }
    }

    // Text cursor over a drawn body. The log's text is not an ImGui item,
    // so this is a raw mouse query; it only applies while nothing modal
    // covers the log and no selection is in progress.
    if (!anyModal && !panelHovered && !sel.active) {
        for (const App::UiMessage& m : app.uiMessages) {
            if (!LayoutFresh(m))
                continue;
            bool over = false;
            for (const RichTextLine& l : m.richLayout.lines)
                if (mouse.y >= l.y0 && mouse.y < l.y1) {
                    over = true;
                    break;
                }
            if (over) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
                return;
            }
        }
    }
}

void DrawMessageLog(App& app) {
    const ImGuiStyle& st = ImGui::GetStyle();
    float inputBarH = InputBarHeight(app.inputBuf);
    // Bottom stack: message log, status strip, input bar, status panel ->
    // four fixed children plus three item spacings between them.
    float bottomH = inputBarH + StatusStripHeight() * 2 + st.ItemSpacing.y * 3;
    float h = std::max(ImGui::GetContentRegionAvail().y - bottomH, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * gFontScale, 10.0f * gFontScale));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 16.0f * gFontScale);
    ImGui::BeginChild("##messages",
                      ImVec2(0, h),
                      ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_None);

    // Raw draw-list calls (RenderRichText, tool rows) are not clipped by
    // ImGui; clip them to the child window bounds so scrolled lines do not
    // bleed into the top bar.
    ImVec2 msgPos = ImGui::GetWindowPos();
    ImVec2 msgSz = ImGui::GetWindowSize();
    // Scroll targets take effect one frame late (ImGui applies them at the
    // next Begin()). While pinned to the bottom, on the frame a new line
    // grows the content the stale scroll draws the newest line one line
    // lower than it will settle at -- flush against the child's bottom edge,
    // touching the status strip below -- before it is corrected next frame.
    // In steady state the last line ends exactly at childBottom - WindowPadding.y
    // (SetScrollHereY snaps it there), so the bottom WindowPadding strip is
    // always empty. Clip the draws to that line so only the transient line is
    // hidden. PushClipRect also clips the ImGui::Text labels, so the misplaced
    // label is hidden with its body.
    const float maxScroll = ImGui::GetScrollMaxY();
    const float clipBottom =
        (maxScroll > 0.0f && app.stickToBottom)
            ? msgPos.y + msgSz.y - ImGui::GetStyle().WindowPadding.y
            : msgPos.y + msgSz.y;
    ImGui::PushClipRect(msgPos, ImVec2(msgPos.x + msgSz.x, clipBottom), true);

    if (app.uiMessages.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
        ImGui::TextUnformatted("No messages yet. Type a prompt below.");
        ImGui::PopStyleColor();
    }

    // Scaffold-run view: while a dispatch is live, the log shows only that
    // dispatch's rows -- every dispatch that starts gets a clean page. The
    // pinned scaffoldViewDispatch (click an entry in the subagent-log
    // column) swaps in an earlier dispatch's transcript; while the parent
    // itself talks (before the first dispatch, the final report) and outside
    // a run the full log shows. Filtering happens here only: uiMessages
    // keeps accumulating every row, so switching views (and the run's end)
    // just changes what is drawn. Rows carry the dispatch id (their index
    // into scaffoldRoleLog), so repeated roles (manager between worker
    // rounds) are told apart.
    const int viewDispatch = app.scaffoldDisplayDispatch();
    std::vector<std::size_t> viewIdx;
    if (viewDispatch >= 0) {
        viewIdx.reserve(app.uiMessages.size());
        for (std::size_t i = 0; i < app.uiMessages.size(); ++i)
            if (app.uiMessages[i].dispatch == viewDispatch)
                viewIdx.push_back(i);
        if (viewIdx.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
            ImGui::TextUnformatted("No messages from this run yet.");
            ImGui::PopStyleColor();
        }
    }
    const std::size_t viewCount = viewDispatch < 0 ? app.uiMessages.size() : viewIdx.size();

    // Virtualize the log. Each message caches the total cursor advance it
    // consumed in its last drawn frame (advanceCache, -1 = unknown). Messages
    // fully outside the viewport that already have a cache only reserve that
    // height with one Dummy -- no text measuring, no pixels -- so the frame
    // cost is bounded by the visible band instead of the whole session. The
    // first frame after load/resize still draws (and thus measures) every
    // message; later frames draw only ~2 viewports worth.
    const float padTop = st.WindowPadding.y;
    const float viewTop = padTop + ImGui::GetScrollY();
    const float viewBot = viewTop + h;
    const float viewMarg = h * 0.5f;
    const float contentW = ImGui::GetWindowContentRegionMax().x;
    if (app.msgLayoutWidth != contentW) {
        app.msgLayoutWidth = contentW;
        app.invalidateMessageAdvances();
    }

    static FILE* gTrace = nullptr;
    if (const char* tp = std::getenv("MINIAGENT_TRACE")) {
        if (!gTrace) gTrace = fopen(tp, "w");
        static int sFrame = 0;
        const int frame = ++sFrame;
        if (frame % 10 == 1) fprintf(gTrace, "--- frame %d scrollY=%.1f maxScroll=%.1f viewTop=%.1f viewBot=%.1f msgs=%zu\n", frame, ImGui::GetScrollY(), maxScroll, viewTop, viewBot, app.uiMessages.size());
    }

    float yTop = ImGui::GetCursorPosY();
    for (size_t vi = 0; vi < viewCount; vi++) {
        const size_t i = viewDispatch < 0 ? vi : viewIdx[vi];
        App::UiMessage& m = app.uiMessages[i];

        const bool known = m.advanceCache >= 0.0f;
        const bool vis = yTop < viewBot + viewMarg &&
                         (m.advanceCache < 0.0f || yTop + m.advanceCache > viewTop - viewMarg);
        if (!vis && known) {
            if (gTrace) fprintf(gTrace, "  [%2zu] %d reserve adv=%.1f yTop=%.1f (vt=%.1f vb=%.1f)\n", i, (int)m.kind, m.advanceCache, yTop, viewTop, viewBot);
            // Off-viewport: reserve the cached height, skip the draw. The
            // Dummy keeps the content height (scrollbar range) correct, and
            // the SetCursorPosY re-anchors the cursor, because the Dummy
            // itself advances it by one extra ItemSpacing.
            ImGui::SetCursorPosY(yTop);
            ImGui::Dummy(ImVec2(1.0f, m.advanceCache));
            yTop += m.advanceCache;
            continue;
        }

        // Re-anchor after a skipped run drifted the cursor.
        if (ImGui::GetCursorPosY() != yTop)
            ImGui::SetCursorPosY(yTop);

        ImVec4 color = ColorForKind(m.kind);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Turns whose assistant message is empty (tool call only) draw
        // nothing but the tool bar.
        if (m.kind == App::UiMessage::Agent && m.text.empty()) {
            m.advanceCache = 0.0f;
            continue;
        }

        if (m.kind == App::UiMessage::Tool) {
            // Tool calls render as a green bubble, same treatment as the user
            // bubble: white mono name + args on a rounded, content-sized
            // background; errors use the same treatment in red.
            const float padX = 8.0f * gFontScale, padY = 3.0f * gFontScale;
            ImFont* mfont = gFontMono ? gFontMono : gFontDefault;
            float msz = gFontMono ? kSizeMono * gFontScale : kSizeDefault * gFontScale;
            float textH = mfont->CalcTextSizeA(msz, FLT_MAX, 0.0f, "Ag").y;
            ImVec2 o = ImGui::GetCursorScreenPos();
            float rowW = ImGui::GetContentRegionAvail().x;

            const viewrows::ToolRowElapsed el = viewrows::toolRowElapsed(
                m.toolStreaming, m.toolDone, m.toolStartMs, m.toolElapsedMs, nowMs());
            const bool toolRunning = el.running;
            const int64_t elapsedMs = el.elapsedMs;
            const bool slow = el.slow;
            const bool elapsedLine = el.line;

            std::string cmdText;
            const bool showCmd =
                m.toolExpanded && m.toolName == "bash" && !m.toolCommand.empty();
            float cmdH = 0.0f;
            if (showCmd) {
                cmdText = "$ " + m.toolCommand;
                cmdH = RenderRichText(ImVec2(0, 0),
                                      rowW - padX * 2,
                                      cmdText,
                                      true,
                                      0,
                                      false,
                                      true,
                                      nullptr,
                                      nullptr);
            }
            float contentH = textH;
            if (showCmd)
                contentH += 2.0f * gFontScale + cmdH;
            if (elapsedLine)
                contentH += textH;
            float rowH = contentH + 2.0f * padY;

            // Status suffix ("[streaming...]" / "[running...]" / "[error]",
            // or "[12.3s]" once a finished tool turns out to have been slow)
            // is never truncated; only the args part gets an ellipsis when the
            // bubble is too wide. Both live in view_rows (shared with the TUI).
            const std::string statusStr =
                viewrows::toolRowStatusSuffix(m.toolStreaming, el, m.toolDone, m.toolIsError);
            const char* status = statusStr.c_str();
            // Collapsed read/write/edit/bash rows: a short summary (the file
            // path, or the first part of the command) instead of the one-line
            // JSON args summary, via the shared row model. The caches in the
            // message (toolFile/toolCommand/...) are updated in place.
            std::string args = viewrows::toolArgsSummary(
                m.toolName,
                m.toolArgs,
                m.toolExpanded,
                m.toolStreaming,
                {m.toolFile, m.toolCommand, m.toolFileResolved, m.toolFileScanSize});
            if (!args.empty())
                args.insert(args.begin(), ' ');
            char tail[1024];
            snprintf(tail, sizeof(tail), "%s%s", args.c_str(), status);
            ImFont* nameFont = (gFontMono && gFontMonoBold) ? gFontMonoBold : mfont;
            float wName = nameFont
                             ->CalcTextSizeA(msz,
                                            FLT_MAX,
                                            0.0f,
                                            m.toolName.c_str(),
                                            m.toolName.c_str() + m.toolName.size())
                             .x;
            const float wStatus =
                mfont->CalcTextSizeA(msz, FLT_MAX, 0.0f, status, status + strlen(status)).x;
            float wTail = (!m.toolStreaming || args.size() <= 4096)
                              ? mfont->CalcTextSizeA(msz,
                                                     FLT_MAX,
                                                     0.0f,
                                                     args.c_str(),
                                                     args.c_str() + args.size())
                                                     .x +
                                  wStatus
                              : FLT_MAX;  // in-flight args beyond the window: guaranteed overflow

            const bool expandable = viewrows::toolRowExpandable(
                m.toolDone, m.toolStreaming, m.toolResult, m.toolName, m.toolCommand);
            float wMark = 0.0f;
            if (expandable) {
                const char* mark = m.toolExpanded ? "\u2212" : "+";  // - / +
                wMark = mfont->CalcTextSizeA(msz, FLT_MAX, 0.0f, mark, mark + 1).x + 6.0f * gFontScale;
            }
            if (wName + wTail + wMark > rowW) {
                // Long args: truncate with an ellipsis so the bubble fits the
                // message area.
                std::string a = args;
                float wEll = mfont->CalcTextSizeA(msz, FLT_MAX, 0.0f, "\u2026", "\u2026" + 3).x;
                if (m.toolStreaming) {
                    // Still growing: keep the newest part visible, so truncate
                    // from the front instead of the tail. Cap the measured
                    // window so a huge in-flight file does not force a full
                    // re-measure every frame; the window is far wider than the
                    // bubble, so only the prefix gets dropped.
                    const size_t kMaxWindow = 4096;
                    if (a.size() > kMaxWindow)
                        a.erase(0, a.size() - kMaxWindow);
                    auto fits = [&](size_t skip) {
                        const char* p = a.c_str() + skip;
                        return wName +
                                   mfont->CalcTextSizeA(msz, FLT_MAX, 0.0f, p, p + (a.size() - skip))
                                       .x +
                                   wEll + wStatus + wMark <=
                               rowW;
                    };
                    if (fits(0)) {
                        // Fits as-is (window clamp alone was enough).
                    } else if (!fits(a.size())) {
                        a.clear();  // even an empty tail does not fit: name only
                    } else {
                        // Binary search the largest front prefix to drop.
                        size_t lo = 0, hi = a.size();
                        while (lo + 1 < hi) {
                            const size_t mid = (lo + hi) / 2;
                            if (fits(mid))
                                hi = mid;
                            else
                                lo = mid;
                        }
                        a.erase(0, hi);
                    }
                    if (!a.empty())
                        a = std::string("\u2026") + a;
                } else {
                    // Settled text: keep the beginning; binary search the
                    // longest prefix that fits (a per-char pop_back was
                    // O(n^2) per frame).
                    auto fits = [&](size_t n) {
                        return wName +
                                   mfont->CalcTextSizeA(msz,
                                                        FLT_MAX,
                                                        0.0f,
                                                        a.c_str(),
                                                        a.c_str() + n)
                                       .x +
                                   wEll + wStatus + wMark <=
                               rowW;
                    };
                    if (fits(0)) {
                        size_t lo = 0, hi = a.size();
                        while (lo + 1 < hi) {
                            const size_t mid = (lo + hi) / 2;
                            if (fits(mid))
                                lo = mid;
                            else
                                hi = mid;
                        }
                        a.resize(lo);
                    } else {
                        a.clear();  // even an empty tail does not fit: name only
                    }
                    a += "\u2026";
                }
                args = a;
                wTail = mfont->CalcTextSizeA(msz,
                                             FLT_MAX,
                                             0.0f,
                                             args.c_str(),
                                             args.c_str() + args.size())
                            .x +
                        wStatus;
                snprintf(tail, sizeof(tail), "%s%s", args.c_str(), status);
            }

            // Same saturated, 0.65-alpha family as the prompt box's active blue:
            // tint by tool kind so a run of calls reads at a glance -- blue for
            // read (a look), amber for write/edit (a mutation), green for bash
            // and everything else; red for errors.
            auto toolTint = [](const std::string& name) -> ImU32 {
                if (name == "read" || name == "grep" || name == "web_search" ||
                    name == "web_fetch")
                    return IM_COL32(30, 80, 150, 166);
                if (name == "edit" || name == "write")
                    return IM_COL32(150, 100, 40, 166);
                return IM_COL32(40, 120, 60, 166);
            };
            ImU32 bg = m.toolIsError ? IM_COL32(150, 58, 54, 166) : toolTint(m.toolName);
            // Anchor the bubble on the cursor line (no stick-up above), so the
            // gap left above equals the gap left below: the surrounding
            // Dummy+Spacing is symmetric, and rowH (textH + 2*padY) already
            // pads the text equally on both sides inside the rect.
            const ImVec2 bMin = ImVec2(o.x - padX, o.y);
            // Full-width bubble; the expand mark sits at the right edge.
            const ImVec2 bMax = ImVec2(o.x + rowW + padX, o.y + rowH);
            dl->AddRectFilled(bMin, bMax, bg, 4.0f * gFontScale);

            const float ty = o.y + padY;
            float tx = o.x;
            dl->AddText(nameFont,
                        msz,
                        ImVec2(tx, ty),
                        IM_COL32(255, 255, 255, 255),
                        m.toolName.c_str(),
                        m.toolName.c_str() + m.toolName.size());
            tx += wName;
            if (tail[0])
                dl->AddText(mfont, msz, ImVec2(tx, ty), IM_COL32(232, 232, 236, 255), tail);
            const float cmdY = ty + textH + 2.0f * gFontScale;
            if (showCmd)
                RenderRichText(ImVec2(o.x, cmdY),
                               rowW - padX * 2,
                               cmdText,
                               true,
                               IM_COL32(232, 232, 236, 255),
                               false,
                               false,
                               nullptr,
                               nullptr);
            if (elapsedLine) {
                char timeLine[48];
                snprintf(timeLine,
                         sizeof(timeLine),
                         "%c %s",
                         viewrows::toolSpinner(elapsedMs),
                         FormatDurationMs(elapsedMs).c_str());
                dl->AddText(mfont,
                            msz,
                            ImVec2(o.x, showCmd ? cmdY + cmdH : ty + textH),
                            IM_COL32(232, 232, 236, 170),
                            timeLine);
            }
            if (expandable) {
                const char* mark = m.toolExpanded ? "\u2212" : "+";  // - / +
                float markW = mfont->CalcTextSizeA(msz, FLT_MAX, 0.0f, mark, mark + 1).x;
                dl->AddText(mfont, msz, ImVec2(o.x + rowW - markW, ty), IM_COL32(255, 255, 255, 190), mark, mark + 1);
            }

            ImGui::SetCursorScreenPos(bMin);
            ImGui::PushID((int)i);
            ImGui::InvisibleButton("##tool", ImVec2(bMax.x - bMin.x, bMax.y - bMin.y));
            ImGui::PopID();
            if (expandable && ImGui::IsItemClicked()) {
                m.toolExpanded = !m.toolExpanded;
                m.advanceCache = -1.0f;
            }

            if (expandable) {
                if (m.toolExpanded) {
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    size_t bodyOff = 0;
                    if (showCmd) {
                        const std::string prefix = "$ " + m.toolCommand + "\n";
                        if (m.toolResult.compare(0, prefix.size(), prefix) == 0)
                            bodyOff = prefix.size();
                    }
                    const std::string body =
                        bodyOff ? m.toolResult.substr(bodyOff) : m.toolResult;
                    m.richTextOffset = (int)bodyOff;
                    float rh = 0.0f;
                    if (!body.empty())
                        rh = RenderRichText(ImVec2(p.x + padX, p.y + 2.0f * gFontScale),
                                             rowW - padX * 2,
                                             body,
                                             true,
                                             IM_COL32(160, 165, 170, 255),
                                             false,
                                             false,
                                             nullptr,
                                             &m.richLayout);
                    if (rh > 0)
                        ImGui::Dummy(ImVec2(1.0f, rh + 2.0f * gFontScale));

                    // read tool: render the image file from disk below the text.
                    ImageTex* tex = !m.toolImagePath.empty() ? getImageTex(m.toolImagePath) : nullptr;
                    if (tex) {
                        float maxW = rowW - padX * 2;
                        float maxH = 360.0f * gFontScale;
                        float s = std::min(1.0f,
                                           std::min(maxW / (float)tex->width,
                                                    maxH / (float)tex->height));
                        ImVec2 sz((float)tex->width * s, (float)tex->height * s);
                        if (sz.y > ImGui::GetTextLineHeightWithSpacing())
                            ImGui::Spacing();
                        ImGui::PushStyleVar(ImGuiStyleVar_ImageBorderSize,
                                            std::max(1.0f, ImGui::GetStyle().ImageBorderSize));
                        ImGui::ImageWithBg(ImTextureRef(tex->gl),
                                           sz,
                                           ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                                           ImVec4(0.10f, 0.10f, 0.12f, 1.0f));
                        ImGui::PopStyleVar();
                    }
                }
            }
        } else if (m.kind == App::UiMessage::Thinking) {
            const bool nested = !m.nestedAgent.empty();
            if (!app.showThinking) {
                // Hidden: single grey "Thinking..." placeholder line.
                if (nested) {
                    std::string ph = m.nestedAgent + " thinking...";
                    ImGui::TextDisabled("%s", ph.c_str());
                } else {
                    ImGui::TextDisabled("Thinking...");
                }
            } else {
                // Grey italic block, no label. A nested (subagent / scaffold
                // role) block gets a one-line role label above it.
                std::string text = TrimMessage(m.text);
                if (!text.empty()) {
                    m.richTextOffset = (int)m.text.find_first_not_of(" \t\r\n");
                    if (nested) {
                        ImGui::TextDisabled("%s thinking", m.nestedAgent.c_str());
                        // Match the gap the previous bubble leaves above the
                        // label, so the label-to-block gap equals the
                        // above-the-label gap.
                        ImGui::Dummy(ImVec2(1.0f, 10.0f * gFontScale));
                    }
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    float availW = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
                    float h = RenderRichText(p,
                                              availW,
                                              text,
                                              true,
                                              (ImU32)ImColor(ImVec4(0.52f, 0.53f, 0.56f, 1.0f)),
                                              true,
                                              false,
                                              nullptr,
                                              &m.richLayout);
                    if (h > 0)
                        ImGui::Dummy(ImVec2(1.0f, h));
                }
            }
        } else {
            // Anchor all positions to the line start so the layout does not
            // depend on SameLine()/Indent() cursor side effects.
            ImVec2 lineStart = ImGui::GetCursorScreenPos();
            if (lineStart.x < ImGui::GetWindowPos().x)
                ImGui::SetCursorPosX(0.0f);
            lineStart = ImGui::GetCursorScreenPos();
            const char* label = m.queued ? "Queued" : LabelForKind(m.kind);
            float labelW = 0.0f;
            if (label[0] != '\0') {
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::Text("%s", label);
                labelW = ImGui::GetItemRectMax().x - lineStart.x;
                ImGui::PopStyleColor();
            }
            float rightX = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - 4.0f;
            if (!m.text.empty()) {
                // Label and message share the same line.
                ImVec2 p = ImVec2(lineStart.x + labelW + (label[0] ? 12.0f : 0.0f), lineStart.y);
                float availW = std::max(rightX - p.x, 1.0f);
                // User messages render as a bubble in a saturated dark blue with
                // white text; agent output stays mono and
                // light grey, so it remains distinguishable without a label. Trim the
                // agent text so a leading/trailing newline does not draw an empty line.
                float h;
                float bubblePadY = 0.0f;
                if (m.kind == App::UiMessage::Agent) {
                    std::string text = TrimMessage(m.text);
                    // Record the layout so agent output is drag-selectable
                    // like thinking blocks; offsets are relative to the
                    // trimmed body.
                    m.richTextOffset = text.empty() ? 0 : (int)m.text.find_first_not_of(" \t\r\n");
                    h = RenderRichText(p,
                                       availW,
                                       text,
                                       true,
                                       IM_COL32(215, 215, 220, 255),
                                       false,
                                       false,
                                       nullptr,
                                       &m.richLayout);
                } else if (m.kind == App::UiMessage::User) {
                    const float bpadX = 8.0f * gFontScale, bpadY = 3.0f * gFontScale;
                    float bw = 0.0f;
                    // Full-width bubble, same treatment as the tool rows: the
                    // background spans the whole message area even when the
                    // prompt is short.
                    float rightEdge = lineStart.x + ImGui::GetContentRegionAvail().x + bpadX;
                    // Measure first so the background can be drawn before the text.
                    h = RenderRichText(p,
                                       availW,
                                       m.text,
                                       true,
                                       IM_COL32(255, 255, 255, 255),
                                       false,
                                       true,
                                       &bw);
                    ImGui::GetWindowDrawList()->AddRectFilled(
                        ImVec2(p.x - bpadX, p.y - bpadY),
                        ImVec2(rightEdge, p.y + h + bpadY),
                        IM_COL32(20, 60, 120, 255),
                        4.0f * gFontScale);
                    // Full text, untrimmed; record the layout for drag-select.
                    m.richTextOffset = 0;
                    h = RenderRichText(p,
                                       availW,
                                       m.text,
                                       true,
                                       IM_COL32(255, 255, 255, 255),
                                       false,
                                       false,
                                       nullptr,
                                       &m.richLayout);
                    bubblePadY = 2.0f * bpadY;
                } else {
                    // Error/System bodies: full text, proportional font, colored.
                    m.richTextOffset = 0;
                    h = RenderRichText(p,
                                       availW,
                                       m.text,
                                       false,
                                       (ImU32)ImColor(color),
                                       false,
                                       false,
                                       nullptr,
                                       &m.richLayout);
                }
                // With a label, its ImGui::Text call already consumed one line, so the
                // Dummy only reserves the extra lines. Without a label no line was
                // consumed, so reserve the full height.
                float totalH = h + bubblePadY;
                float reserve = label[0] ? std::max(0.0f, totalH - ImGui::GetTextLineHeight())
                                         : std::max(0.0f, totalH);
                ImGui::Dummy(ImVec2(1.0f, reserve));
            } else {
                ImGui::SameLine();
                ImGui::TextDisabled("...");
            }
        }
        // Bubbles (user / tool) and subagent blocks get more breathing room
        // from the surrounding text: add a gap when exactly one of the two
        // neighboring messages is a bubble. Back-to-back bubbles stack flush
        // (no Dummy, no Spacing), so a run of tool calls reads as one list.
        // Rows of one subagent run (header, thinking, text, tools of the same
        // role) keep their usual tight spacing.
        const bool curBubble =
            m.kind == App::UiMessage::User || m.kind == App::UiMessage::Tool ||
            !m.nestedAgent.empty();
        bool nextBubble = false;
        bool sameSubagentRun = false;
        // In a dispatch view the neighbor is the next row of that view, not
        // of the full log (the skipped rows take no space).
        if (viewDispatch < 0 ? i + 1 < app.uiMessages.size() : vi + 1 < viewCount) {
            const App::UiMessage& n =
                app.uiMessages[viewDispatch < 0 ? i + 1 : viewIdx[vi + 1]];
            nextBubble =
                n.kind == App::UiMessage::User || n.kind == App::UiMessage::Tool ||
                !n.nestedAgent.empty();
            sameSubagentRun = !m.nestedAgent.empty() && m.nestedAgent == n.nestedAgent;
        }
        const bool flushPair = curBubble && nextBubble && !sameSubagentRun;
        if ((curBubble || nextBubble) && !sameSubagentRun && !flushPair)
            ImGui::Dummy(ImVec2(1.0f, 10.0f * gFontScale));
        if (!flushPair)
            ImGui::Spacing();

        float yAfter = ImGui::GetCursorPosY();
        m.advanceCache = yAfter - yTop;
        if (gTrace && (m.kind == App::UiMessage::Tool || (m.kind == App::UiMessage::Agent && !m.text.empty())))
            fprintf(gTrace, "  [%2zu] %d drawn adv=%.1f yTop=%.1f->%.1f\n", i, (int)m.kind, m.advanceCache, yTop, yAfter);
        yTop = yAfter;
    }

    // A just-pinned role review opens at the top of that role's transcript
    // (not stuck to the bottom like the live view).
    if (app.msgScrollToTop) {
        app.msgScrollToTop = false;
        app.stickToBottom  = false;
        ImGui::SetScrollY(0.0f);
    }

    // Track whether the user is pinned to the bottom: scroll up releases the
    // pin, scrolling back to the bottom re-engages it.
    if (ImGui::GetScrollY() < maxScroll - 60.0f)
        app.stickToBottom = false;
    else if (maxScroll > 0.0f && ImGui::GetScrollY() >= maxScroll - 2.0f)
        app.stickToBottom = true;
    if (app.stickToBottom)
        ImGui::SetScrollHereY(1.0f);

    // Sessions button, CSS-absolute style: pinned to this child window's
    // top-right corner, floating over the message body. Drawn last in the
    // child's draw list (on top of the log), it does not scroll with the
    // content and takes no layout space. Opens the session-list modal (the
    // list is no longer a pane). Inset by the scrollbar width so it never
    // covers the scroll handle.
    const float margin = 8.0f * gFontScale;
    const float sbW = 16.0f * gFontScale;  // matches the ScrollbarSize pushed above
    // The dark-green colors and generous padding come from the app-wide
    // style default (main.cpp). Monospace font for the label; width is
    // measured in that font so it is sized right.
    ImGui::PushFont(gFontMono ? gFontMono : ImGui::GetFont());
    const float btnW =
        ImGui::CalcTextSize("Sessions").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float newW =
        ImGui::CalcTextSize("New Session").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float mcpW =
        ImGui::CalcTextSize("MCP").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float btnH = ImGui::GetFrameHeight();
    const float gap  = ImGui::GetStyle().ItemSpacing.x;
    const float baseX = msgPos.x + msgSz.x - sbW - margin;
    // "MCP", leftmost: opens the MCP server list modal (enable/disable
    // servers from ~/.config/miniagent/mcp.json). Disabled while the agent
    // is running, like its two neighbors (the registry is owned by the
    // running agent then).
    if (app.agentRunning) ImGui::BeginDisabled();
    ImGui::SetCursorScreenPos(
        ImVec2(baseX - btnW - gap - newW - gap - mcpW, msgPos.y + margin));
    if (HandButton("MCP", ImVec2(mcpW, btnH)))
        app.mcpModalOpen = true;
    const bool mcpHovered = ImGui::IsItemHovered();
    if (app.agentRunning) ImGui::EndDisabled();
    // "New Session", immediately left of the Sessions button (whose box is
    // [baseX - btnW, baseX]): right edge at its left edge minus the gap.
    // Clicking starts a fresh session; it is a no-op while the agent is
    // running, so reflect that by disabling it then.
    if (app.agentRunning) ImGui::BeginDisabled();
    ImGui::SetCursorScreenPos(
        ImVec2(baseX - btnW - gap - newW, msgPos.y + margin));
    if (HandButton("New Session", ImVec2(newW, btnH)))
        app.newSession();
    const bool newSessionHovered = ImGui::IsItemHovered();
    if (app.agentRunning) ImGui::EndDisabled();
    // Sessions, rightmost. Disabled while the agent is running (same as
    // "New Session"): switching/opening the list mid-run is not allowed.
    if (app.agentRunning) ImGui::BeginDisabled();
    ImGui::SetCursorScreenPos(ImVec2(baseX - btnW, msgPos.y + margin));
    if (HandButton("Sessions", ImVec2(btnW, btnH))) {
        app.refreshSessionList();  // fresh list the moment it opens
        app.sessionsModalOpen = true;
    }
    if (app.agentRunning) ImGui::EndDisabled();
    // The labels are monospace; the tooltips render with the app's default
    // font (this ImGui version draws tooltips inline, so the font at the
    // SetTooltip call determines it).
    ImGui::PopFont();
    if (mcpHovered)
        ImGui::SetTooltip("MCP servers: enable/disable Model Context Protocol servers "
                          "defined in ~/.config/miniagent/mcp.json. Their tools appear as "
                          "mcp_<server>_<tool> in the agent's tool list.");
    if (newSessionHovered)
        ImGui::SetTooltip("Start a new session (Ctrl+N). The current conversation "
                          "is kept in the session list.");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Open the session list: click to switch, middle-click deletes, "
                           "right-click copies the session's first message.");

    // Live scaffold task list: while a scaffold run is in progress, a
    // floating panel under the New Session / Sessions buttons shows the same
    // task list as the task modal, plus a subagent-log column (hidden while
    // the modal itself is open).
    // Right edge aligned with the Sessions button, wide enough for the two
    // columns (task list + role log).
    if (app.scaffoldRunActive && !app.scaffoldTasksOpen) {
        const float panelW = std::max(newW + gap + btnW, 640.0f * gFontScale);
        const float panelH =
            std::min(320.0f * gFontScale, msgSz.y - (margin + btnH + 2.0f * gap) - margin);
        if (panelH > 0.0f)
            DrawScaffoldTaskPanel(app,
                                  ImVec2(baseX - panelW, msgPos.y + margin + btnH + gap),
                                  ImVec2(panelW, panelH));
    }

    // Drag-selection of a message body (left-drag; the selection is
    // copied to the clipboard on release).
    UpdateMessageSelection(app);
    DrawSelectionHighlight(app);

    ImGui::PopClipRect();
    ImGui::EndChild();
    ImGui::PopStyleVar();  // ScrollbarSize
    ImGui::PopStyleVar();  // WindowPadding
}
