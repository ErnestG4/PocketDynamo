// The looper records the event stream, not audio: every NoteOn/NoteOff,
// re-strike, retune, control move and engine change sent to the synth while
// it records is timestamped, then replayed every lap — so arps, ratchets,
// chords and sweeps all loop, and overdubs stack. The first pass sets the
// lap, snapped to whole beats so tempo-synced strips stay in time. After
// Audiothurgist's unquantized event looper (Combust).
//
// Replayed notes use their own tags (live tag + kLoopTagOffset), so a loop
// never steals the gate of the note you're playing over it.
// Times are microseconds on a free-running uint32 clock (wrap-safe). Pure —
// host-testable.
#pragma once
#include <cstdint>

#include "events.h"

namespace pd {

class Looper {
 public:
  static constexpr int kMaxEvents = 1024;
  static constexpr uint8_t kLoopTagOffset = 128;   // live tags are 1..127
  enum State : uint8_t { Empty, Recording, Playing, Overdub, Stopped };

  State state() const { return state_; }
  uint32_t lapUs() const { return lap_; }
  int events() const { return count_; }
  bool full() const { return count_ >= kMaxEvents; }
  // 0..1 through the current lap (or the first pass's elapsed time / 0).
  float position(uint32_t now) const { return lap_ ? (float)((now - start_) % lap_) / (float)lap_ : 0.0f; }
  uint32_t elapsed(uint32_t now) const { return now - start_; }

  // The record key: Empty/Stopped -> Recording -> Playing <-> Overdub.
  // `beatUs` snaps the first lap; `held` are the live tags still sounding
  // when the lap closes (they get a NoteOff at its end so nothing hangs).
  template <typename Emit>
  void press(uint32_t now, uint32_t beatUs, const uint8_t* held, int nHeld, Emit emit) {
    switch (state_) {
      case Empty:
        count_ = 0; start_ = now; state_ = Recording;
        break;
      case Recording: {
        const uint32_t len = now - start_;
        uint32_t beats = (len + beatUs / 2) / beatUs;
        if (beats < 1) beats = 1;
        lap_ = beats * beatUs;
        for (int i = 0; i < nHeld; ++i) add(lap_ - 1, {EventType::NoteOff, held[i], 0, 0.0f});
        sort();
        // Events recorded past the snapped end fold back into the lap.
        for (int i = 0; i < count_; ++i) at_[i] %= lap_;
        sort();
        state_ = Playing;
        cursor_ = 0; lapStart_ = start_ + lap_; playable_ = count_;
        // Closed late (the lap snapped down): what was played since the
        // snapped end was just heard live and folded to the lap's start, so
        // skip past it instead of replaying it at once.
        if ((int32_t)(now - lapStart_) >= 0)
          while (cursor_ < playable_ && at_[cursor_] < now - lapStart_) ++cursor_;
        (void)emit;
        break;
      }
      case Playing: state_ = Overdub; break;
      case Overdub: {   // new events join at the next lap
        // A note still held as overdub ends had its NoteOn recorded but
        // won't have its NoteOff: end it here, or every lap would strike it
        // again and pile up voices.
        const uint32_t t = (now - lapStart_) % lap_;
        for (int i = 0; i < nHeld; ++i) add(t, {EventType::NoteOff, held[i], 0, 0.0f});
        state_ = Playing;
        break;
      }
      case Stopped:
        sort(); playable_ = count_;
        state_ = Playing; start_ = now; lapStart_ = now; cursor_ = 0;
        break;
    }
  }

  // The stop key: playing -> Stopped (with every looped note released);
  // stopped -> Empty.
  template <typename Emit>
  void stop(Emit emit) {
    if (state_ == Stopped || state_ == Recording) { state_ = Empty; count_ = playable_ = 0; lap_ = 0; return; }
    if (state_ == Empty) return;
    // Every tag the loop plays, plus any it left sounding: a stray NoteOff
    // in the loop can clear a tag's sounding bit while another of its
    // voices still rings, so the bits alone can't be trusted. (emit must
    // not drop events: the synth's queue is shorter than 127.)
    uint32_t tags[4] = {sounding_[0], sounding_[1], sounding_[2], sounding_[3]};
    for (int i = 0; i < count_; ++i)
      if (ev_[i].type == EventType::NoteOn && ev_[i].a < kLoopTagOffset)
        tags[ev_[i].a >> 5] |= 1u << (ev_[i].a & 31);
    for (int t = 0; t < 128; ++t)
      if (tags[t >> 5] & (1u << (t & 31))) emit(Event{EventType::NoteOff, (uint8_t)(t + kLoopTagOffset), 0, 0.0f});
    for (auto& w : sounding_) w = 0;
    state_ = Stopped;
  }

  // Every event sent to the synth passes through here.
  void capture(uint32_t now, const Event& e) {
    if (state_ != Recording && state_ != Overdub) return;
    if (e.type == EventType::AllOff) return;
    if (e.a >= kLoopTagOffset && isNote(e.type)) return;   // already a replay
    // Overdubs go in at their place in the lap; they play from the next lap
    // (playable_ only grows at lap boundaries) so you don't hear them twice.
    const uint32_t t = state_ == Recording ? now - start_ : (now - lapStart_) % lap_;
    add(t, e);
  }

  // Call often: emits the events whose time has come.
  template <typename Emit>
  void service(uint32_t now, Emit emit) {
    if (state_ != Playing && state_ != Overdub) return;
    catchUp(now, emit);
  }

 private:
  static bool isNote(EventType t) {
    return t == EventType::NoteOn || t == EventType::NoteOff || t == EventType::Retrigger || t == EventType::Pitch;
  }

  template <typename Emit>
  void catchUp(uint32_t now, Emit emit) {
    for (int guard = 0; guard < 4 * kMaxEvents; ++guard) {
      if (cursor_ >= playable_) {
        // Lap done once its end has passed; overdubs join here.
        if ((int32_t)(now - (lapStart_ + lap_)) < 0) return;
        lapStart_ += lap_; cursor_ = 0;
        if (playable_ != count_) { sort(); playable_ = count_; }
        if (count_ == 0) return;
        continue;
      }
      if ((int32_t)(now - (lapStart_ + at_[cursor_])) < 0) return;
      Event e = ev_[cursor_++];
      if (isNote(e.type) && e.a < kLoopTagOffset) e.a = (uint8_t)(e.a + kLoopTagOffset);
      if (e.a >= kLoopTagOffset) {
        const int t = e.a - kLoopTagOffset;
        if (e.type == EventType::NoteOn) sounding_[t >> 5] |= 1u << (t & 31);
        else if (e.type == EventType::NoteOff) sounding_[t >> 5] &= ~(1u << (t & 31));
      }
      emit(e);
    }
  }

  void add(uint32_t t, const Event& e) {
    if (count_ >= kMaxEvents) return;
    at_[count_] = t; ev_[count_] = e; ++count_;
  }

  // Stable insertion sort by time: events at the same instant keep their
  // order (a NoteOff before the NoteOn that re-strikes the key).
  void sort() {
    for (int i = 1; i < count_; ++i) {
      const uint32_t t = at_[i];
      const Event e = ev_[i];
      int j = i;
      while (j > 0 && at_[j - 1] > t) { at_[j] = at_[j - 1]; ev_[j] = ev_[j - 1]; --j; }
      at_[j] = t; ev_[j] = e;
    }
  }

  uint32_t sounding_[4] = {};   // loop tags with a replayed NoteOn not yet released
  uint32_t at_[kMaxEvents];
  Event ev_[kMaxEvents];
  int count_ = 0;
  int cursor_ = 0;
  int playable_ = 0;   // events sorted into the current lap
  uint32_t start_ = 0, lap_ = 0, lapStart_ = 0;
  State state_ = Empty;
};

}  // namespace pd
