#pragma once

#include <functional>
#include <string>

#include "tools/tools.h"

// The recall tool: searches the current session file (full history including
// everything dropped by compaction). `sessionFile` is queried per call so a
// session switch or the newest messages are always visible. Actions:
//   query           keyword search (all terms substring-matched, ranked)
//   range:[a,b]     entries #a..#b in order
//   expand:[N,...]  full untruncated text of entries #N
//   mode:"touched"  files worked on, with the entries that touched them
// #N is the stable 1-based entry index over the session file (the same
// numbers the compaction summary references as (#N)).
Tool makeRecallTool(std::function<std::string()> sessionFile);
