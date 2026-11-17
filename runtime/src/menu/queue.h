// menu/queue.h: lets the game use Atlas's matchmaker queue (the FIND MATCH bar's estimated time) with any account.
#pragma once

namespace evr_queue {

/// Opens the game's platform gate on the matchmaker queue. Game thread, once, before the first login.
/// estimateScale: test aid, multiplies valid estimates before the bar reads them (1 = unchanged).
void Install(int estimateScale = 1);

}  // namespace evr_queue
