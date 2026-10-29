// The microSD card: patches as text, takes as WAV, in /pocketdynamo/.
// Slot n is pN.txt (+ pN.wav when the patch has a take). Call only from the
// control task; SD access blocks for milliseconds (the audio core doesn't
// notice — it never touches the card).
#pragma once
#include "../synth/sample_store.h"
#include "../ui/patch.h"

namespace pd {
namespace storage {

bool begin();          // mount; false = no card
bool mounted();
void report();         // card type and size to Serial
// Writes the patch, and the take if `take` is ready (else removes a stale WAV).
bool save(int slot, const patch::Patch& p, const SampleStore& take);
// Reads the patch; loads its WAV into `take` if there is one (resampled to
// the store's rate). *tookTake says whether the take changed.
bool load(int slot, patch::Patch* p, SampleStore& take, bool* tookTake);

}  // namespace storage
}  // namespace pd
