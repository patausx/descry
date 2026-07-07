// app state: current view, navigation, input
// views: phrase / chain / song / instrument
#pragma once
#include "draw.h"
#include "../sequencer/project.h"
#include "../sequencer/player.h"
#include "../sequencer/undo.h"

namespace trackr::ui {

enum class Screen : uint8_t {
    Song = 0,
    Chain,
    Phrase,
    Instrument,
    Table,
    Mixer,
    Project,
    NUM
};

// input abstraction - map 3ds buttons to abstract actions
struct InputState {
    // edge-triggered (once per press, with repeat)
    bool up    = false;
    bool down  = false;
    bool left  = false;
    bool right = false;

    bool a = false;
    bool b = false;
    bool x = false;
    bool y = false;

    bool l = false;     // edge: switch screen <-
    bool r = false;     // edge: switch screen ->

    bool start = false;
    bool select_ = false;

    // held - for shift keys
    bool held_l = false;
    bool held_r = false;
    bool held_zl = false;   // ZL held - modifier for copy/paste (ZL+X / ZL+Y)
    bool held_select = false;   // SELECT held - sustains the preview note (gate)

    // === analog sticks (new3ds): left circle pad + right C-stick ===
    // normalized to -1000..+1000 (after deadzone). 0 = center/at rest.
    int  lstick_x = 0, lstick_y = 0;   // left circle pad
    int  cstick_x = 0, cstick_y = 0;   // right C-stick
    bool lstick_active = false;         // left moved out of deadzone
    bool cstick_active = false;         // right moved out of deadzone
};

class App {
public:
    App(seq::Project& p, seq::Player& pl, audio::Mixer& m)
        : project_(p), player_(pl), mixer_(m) {}

    void update(const InputState& in);

    // touch on the bottom screen - separate method, x/y in bottom-screen coords (320x240)
    void touch(int x, int y);          // press (edge)
    void touch_move(int x, int y);     // movement while the finger is held (every frame)
    void touch_release();              // release

    // called every frame to update hover and timings
    void tick();

    // current keyboard octave
    // flags for main to call save/load (gets and clears)
    bool consume_save_request() { bool v = save_request_; save_request_ = false; return v; }
    bool consume_load_request() { bool v = load_request_; load_request_ = false; return v; }
    bool consume_render_request() { bool v = render_request_; render_request_ = false; return v; }
    bool consume_reset_request() { bool v = reset_request_; reset_request_ = false; return v; }
    bool rec_mode() const { return rec_mode_; }
    int  octave() const { return octave_; }

    // where to write the mic recording - depends on the current screen:
    //   Sample editor       -> cur_sample_
    //   Instrument (DrumKit)-> selected pad's slot (if empty - first free in the bank)
    //   Instrument (Sampler)-> inst.sampler.sample_slot
    //   everything else     -> cur_sample_ (slot 0 by default)
    int rec_target_slot() const;

    // after recording the mic into a slot - if we are in DrumKit and the cursor is on an empty pad,
    // write the slot there automatically
    void on_rec_done(int slot);

    // last touched key for keyboard highlight
    int  last_keyboard_note() const { return last_kb_note_; }

    // draw the current view on the top screen (400x240)
    void draw_top(Draw& d);

    // draw the bottom screen helper (320x240) - status, touch keyboard, debug info
    void draw_bottom(Draw& d);

    Screen current_screen() const { return screen_; }

    // cursor in the phrase view (for highlighting)
    int cursor_row() const { return cursor_row_; }
    int cursor_col() const { return cursor_col_; }

    // currently editable ids
    uint8_t current_phrase()    const { return cur_phrase_; }
    uint8_t current_chain()     const { return cur_chain_; }
    uint8_t current_instrument() const { return cur_inst_; }

private:
    // drawing of individual screens
    void draw_song(Draw& d);
    void draw_song_timeline(Draw& d);   // horizontal DAW-style lane view (toggle: ZL+left/right)
    void draw_chain(Draw& d);
    void draw_phrase(Draw& d);
    void draw_instrument(Draw& d);
    // M8-style slice editor panel on the bottom screen (Instrument view, Sampler).
    // draws the waveform + chop markers; touch handled via slice_panel_touch.
    void draw_slice_panel(Draw& d, int slot);
    void slice_panel_touch(int x, int y, int slot, bool is_move);
    // WAVE panel: waveform + start/length drag + destructive op buttons
    // (NORM/REV/FADI/FADO/G+/G-/CROP). replaces the old Sample-screen Trim/Fx modes.
    void draw_wave_panel(Draw& d, int slot);
    void wave_panel_touch(int x, int y, int slot, bool is_move);
    // REC panel: bounce master output into the instrument's sample slot.
    void draw_rec_panel(Draw& d, int slot);
    // M8-style WAV loader/browser panel on the bottom screen (Instrument view, Sampler).
    // draws a scrollable file list; navigated by up/down/A (load_panel_input).
    void draw_load_panel(Draw& d, int slot);
    void load_panel_input(const InputState& in, int slot);
    // KB/SLICE/LOAD tab buttons on the bottom screen (Sampler instrument).
    void draw_inst_tabs(Draw& d);
    bool inst_tab_touch(int x, int y);   // returns true if a tab was hit
    void draw_table(Draw& d);
    void draw_project(Draw& d);
    void draw_scope_fullscreen(Draw& d);

    // navigation update in the current view
    void update_phrase(const InputState& in);
    void update_chain(const InputState& in);
    void update_song(const InputState& in);
    void update_instrument(const InputState& in);
    // edit one row of the Sampler instrument param list (M8-style). inst_row_ selects the row.
    void edit_sampler_row(synth::SamplerParams& sp, seq::Instrument& inst, int delta);
    // edit one row of the DSN instrument param list.
    void edit_dsn_row(synth::DsnSynthParams& dp, seq::Instrument& inst, int delta);
    // push edited instrument params into all live voices of that instrument
    // (held preview notes + playing sequencer notes react to edits instantly).
    void push_live_inst_params(uint8_t inst_id);
    // ADSR envelope popup: pops over the instrument view while the cursor is on
    // an envelope param (wavsynth/sampler/FM op/DSN EG1+EG2). draws the curve
    // with the edited stage highlighted.
    void draw_env_overlay(Draw& d);
    // (re)start the popup slide-in when it first appears after being hidden
    void env_anim_latch() {
        if (!env_popup_on_) { env_popup_on_ = true; env_popup_frame_ = frame_; }
    }
    void draw_env_popup(Draw& d, uint32_t atk, uint32_t dec, fx::q15 sus,
                        uint32_t rel, int focus, const char* title,
                        int live_stage = -1, fx::q15 live_level = 0);
    // per-instrument FX defaults section: navigation/edit (inst_fx_col_) and drawing.
    // shared across all instrument types. returns true if FX handled the input.
    bool update_fx_section(const InputState& in, seq::Instrument& inst);
    void draw_fx_section(Draw& d, const seq::Instrument& inst);
    void update_table(const InputState& in);
    void update_mixer(const InputState& in);
public:
    // push the Song's persisted mixer settings (faders/delay/reverb/duck)
    // into the audio engine. must be called after loading a project -
    // otherwise the settings only kick in when the mixer screen is visited.
    void sync_mixer_from_song();
private:
    void draw_mixer(Draw& d);
    // bottom-screen touch faders for the Mixer view (9 strips: 8 tracks + master)
    void draw_mixer_faders(Draw& d);
    void mixer_fader_touch(int x, int y, bool is_move);
    void update_project(const InputState& in);

    // helper: change the value under the cursor (b = -1, a = +1, with modifier = big step)
    void edit_value(int delta);

    // quick erase in the phrase view (held_R + B/A)
    void clear_step(int row);            // whole step -> default
    void clear_cell(int row, int col);   // single cell under the cursor

    // === undo/redo ===
    seq::UndoStack undo_;
    // capture the phrase step under (cur_phrase_, row) before an edit; record after.
    // usage: snapshot_step(row) -> mutate -> commit_step(row).
    seq::EditRecord::Payload step_before_;
    bool step_snap_taken_ = false;
    void snapshot_step(int row);
    void commit_step(int row);
    void do_undo();
    void do_redo();

    // create a DrumKit instrument from the chops of the current sample.
    // if there are no chops - auto-slice into 16 equal pieces.
    // returns the inst id of the created kit, or -1 on error.
    // updates smp_status_ with the result.
    int make_kit_from_sample(int sample_slot);

    // create a Sampler instrument from the sample. returns inst id or -1.
    int make_sampler_inst_from_sample(int sample_slot);

    // === stick live-mod indicator state (set in update, read in draw_top) ===
    bool perf_lstick_on_ = false;    // left circle pad active this frame
    bool perf_cstick_on_ = false;    // right C-stick active this frame
    int  perf_cutoff_ = 0;           // current values being modulated (0..100 for display)
    int  perf_reso_ = 0;
    int  perf_send_ = 0;
    int  perf_bits_ = 16;

    // === clipboard for copy/paste (ZL+X copy step, ZL+Y paste) ===
    seq::PhraseStep clipboard_step_;
    bool has_clip_ = false;

    // === clone feedback ("CLONE 1F" / "BANK FULL" toast in song/chain views) ===
    // set by ZL+SELECT clone in update_song/update_chain; fades after ~1.5s.
    char     clone_msg_[16] = {0};
    uint32_t clone_msg_frame_ = 0;

    // === selection mode (m8-style block copy/paste in the phrase view) ===
    // ZL+SELECT (phrase) enters selection: sel_anchor_ = cursor row, moving the
    // cursor extends the [anchor..cursor] row range. A/ZL+X = copy rows + exit,
    // B = cancel. ZL+Y pastes the block clipboard at the cursor.
    bool sel_mode_ = false;
    int  sel_anchor_ = 0;
    seq::PhraseStep clip_block_[seq::PHRASE_STEPS];
    int  clip_block_len_ = 0;

    // === FX help view (m8-style command picker) ===
    // active while the phrase cursor sits on an FX-cmd column AND fx_help_ is on
    // (toggled with SELECT on the fx column). bottom screen lists all commands
    // with names; tap or up/down+A assigns to the current slot.
    bool fx_help_ = false;
    int  fx_help_sel_ = 0;
    void draw_fx_help(Draw& d);
    bool fx_help_touch(int x, int y);

    // === animation layer (frame-based) ===
    uint32_t frame_ = 0;             // grows every tick()
    // held-modifier mirror for the bottom-screen hint bar (set in update())
    bool mod_l_ = false, mod_r_ = false, mod_zl_ = false;
    uint32_t edit_flash_frame_ = 0;  // frame of the last value edit (value-flash)
    static constexpr uint32_t FLASH_FRAMES = 6;  // flash duration ~100ms @60fps
    int last_playing_step_ = -1;     // for detecting a step change (playhead pulse)
    uint32_t step_change_frame_ = 0; // when the playhead last moved to a new step
    uint32_t screen_change_frame_ = 0; // when the screen last switched (nav slide-in)
    int      nav_dir_ = 0;            // +1 = moved right (R), -1 = moved left (L); slide direction
    uint8_t  prev_screen_ = 0;        // screen we came from (nav capsule slides from its icon)

    // mixer peak-hold markers (UI-side): hold the highest recent meter value per
    // track, decay slowly. classic falling caps on both mixer meter columns.
    fx::q15  peak_hold_[8] = {0};

    // kaoss tap ripple: expanding ring from the touch point on gesture start
    int16_t  kaoss_rip_x_ = -1, kaoss_rip_y_ = -1;
    uint32_t kaoss_rip_frame_ = 0;
    uint32_t kaoss_menu_frame_ = 0;   // when the assign popup opened (unfold anim)
    uint32_t fx_help_frame_ = 0;      // when the fx help popup opened (unfold anim)

    // song view playhead trail: per-track previous row + when it moved
    // (the old cell ghosts out over ~24 frames)
    uint16_t song_ph_prev_[8]  = {0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF};
    uint16_t song_ph_row_[8]   = {0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF};
    uint32_t song_ph_frame_[8] = {0};
    static constexpr uint32_t NAV_SLIDE_FRAMES = 10; // slide-in duration ~165ms @60fps

    // value-flash brightness 0..255 for the current frame (0 = no flash)
    uint8_t edit_flash_t() const {
        uint32_t d = frame_ - edit_flash_frame_;
        if (edit_flash_frame_ == 0 || d >= FLASH_FRAMES) return 0;
        return (uint8_t)(255 - (d * 255 / FLASH_FRAMES));
    }

    seq::Project& project_;
    seq::Player&  player_;
    audio::Mixer& mixer_;

    Screen  screen_ = Screen::Phrase;

    // cursor in the phrase view (16 rows x 5 cols: NOTE/INST/FX1/FX2/FX3 - but for compact 3 cols)
    int cursor_row_ = 0;
    int cursor_col_ = 0;     // 0=note, 1=inst, 2=vel, 3=fx1cmd, 4=fx1val, 5=fx2cmd, 6=fx2val, 7=fx3cmd, 8=fx3val

    // navigation in the chain view (16 rows x 2 cols: phrase/transpose)
    int chain_row_ = 0;
    int chain_col_ = 0;

    // navigation in the song view (256 rows x 8 tracks)
    int song_row_ = 0;
    int song_col_ = 0;       // 0..7 = track
    int solo_track_ = -1;    // song-view SOLO (-1 = none): solo mutes all others

    // navigation in the instrument view
    int inst_row_ = 0;       // 0=type, 1=shape, 2=attack, 3=decay, 4=sustain, 5=release ...
    int inst_col_ = 0;       // for FM: 0=ratio 1=level 2=attack 3=decay 4=sustain 5=release
    // per-instrument FX defaults section (filter/send/vol/pan/crush). shared by all
    // instrument types (fields live in seq::Instrument, not the type union).
    // -1 = section NOT focused (cursor in the normal rows). 0..6 = selected FX cell.
    // toggled with ZL+SELECT. while >=0, FX takes over left/right + A/B/X/Y editing.
    int inst_fx_col_ = -1;
    int fm_preset_idx_ = 0;  // current FM preset in the applied slot (for cycling)
    int wav_preset_idx_ = 0; // current wavsynth preset (cycling)
    int dsn_preset_idx_ = 0; // current DSN preset (cycling)

    // editable objects
    uint8_t cur_phrase_ = 0;
    uint8_t cur_chain_  = 0;
    uint8_t cur_inst_   = 0;
    uint8_t cur_sample_ = 0;     // selected sample slot (rec target outside instrument views)
    uint8_t cur_table_  = 0;     // selected table for the editor

    // === sample panel state (WAVE/SLICE/LOAD/REC bottom panels of Instrument) ===
    int        smp_chop_sel_   = 0;        // selected chop 0..15 (SLICE panel)
    int        wav_sel_        = 0;        // selected WAV file in LOAD panel
    int        wav_count_      = 0;        // number of scanned WAVs
    bool       wav_scanned_    = false;    // flag: the list has already been scanned
    static constexpr int MAX_WAV_FILES = 64;
    char       wav_files_[MAX_WAV_FILES][40] = {{0}};
    bool       wav_is_dir_[MAX_WAV_FILES] = {false};   // true = this is a subfolder (enter with A)
    char       wav_subpath_[80] = {0};                 // current subfolder relative to wav/ ("" = root)
    int        smp_drag_kind_  = 0;        // 0=none,1=start,2=end,3=loop_s,4=loop_e,5=chop
    bool       smp_touch_active_ = false;
    char       smp_status_[40] = {0};      // last action (for UI)
    int     table_row_ = 0;      // cursor row in the table view
    int     table_col_ = 0;      // 0..5 = 3 fx slots * (cmd, val)
    int     proj_slot_ = 0;      // selected slot in the project menu (0..15)
    int     rename_pos_ = 0;     // cursor pos in project rename mode (0..23)
    // mixer view cursor: col 0..7 = tracks, 8 = master strip; row 0=vol 1=mute (tracks)
    // master strip rows: 0=master 1=dly time 2=dly fb 3=dly wet 4=rev wet
    int     mixer_col_ = 0;
    int     mixer_row_ = 0;
    bool    mixer_touch_active_ = false;   // finger dragging a bottom-screen fader

    // touch keyboard
    int     octave_ = 4;            // from 0 to 8
    int     last_kb_note_ = -1;     // last played note (for highlighting)
    int     last_note_entered_ = 60; // sticky note entry: A on empty step inserts THIS (m8-style)
    int     touch_held_note_ = -1;  // note while the finger is held (for sustain)
    bool    preview_gate_ = false;  // SELECT preview held: gate off on release (hold-to-sustain)
    bool    song_timeline_ = false; // song view orientation: false = vertical list, true = horizontal timeline
    // env popup slide-in animation: frame the popup (re)appeared + visibility latch
    uint32_t env_popup_frame_ = 0;
    bool     env_popup_on_    = false;
    uint32_t pad_flash_frame_ = 0;  // frame of the last pad hit (press-flash anim)
    // bottom input mode: piano keyboard / 4x4 performance pads / KAOSS XY pad.
    // cycled by the KEYS->PADS->KAOSS button (and its touch hotspot).
    enum class KbMode : uint8_t { Keys = 0, Pads, Kaoss };
    KbMode  kb_mode_ = KbMode::Keys;
    // resolve a touch to a midi note honoring kb_mode_ (-1 = miss)
    int     touch_note_at(int x, int y) const;
    // touch velocity: Y position within the key (keys) / within the pad cell
    // (pads) -> 1..127. bottom = loud, top = soft.
    int     touch_velocity(int x, int y) const;
    int     touch_vel_ = 110;       // velocity of the held touch (for glissando retrigs)
    // tap tempo state (tapping the BPM readout on the bottom screen)
    uint32_t tap_last_frame_ = 0;

    // === KAOSS pad (XY touch performance controller, dsn-12 heritage) ===
    // the bottom keyboard band becomes an XY field writing into the target
    // track's mixer DSP (same path as the live stick modulation above).
    // each axis has an ASSIGNABLE destination picked from a popup menu
    // (tap the X/Y button in the left column -> grid of destinations).
    enum class KaossDest : uint8_t {
        Cut = 0,   // filter cutoff
        Res,       // filter resonance
        Del,       // delay send
        Rev,       // reverb send
        Bit,       // bitcrush (audible-range curve)
        Dsm,       // downsample 1..6
        Rat,       // MG (LFO) rate
        Mgc,       // MG -> cutoff depth   (signed, center = 0)
        Mgv,       // MG -> volume depth   (signed, center = 0)
        Vol,       // channel fader (mix_vol)
        Pan,       // stereo pan           (signed, center = C)
        COUNT
    };
    KaossDest kaoss_dest_x_ = KaossDest::Cut;
    KaossDest kaoss_dest_y_ = KaossDest::Res;
    int  kaoss_menu_ = 0;           // 0 = closed, 1 = assigning X, 2 = assigning Y
    static const char* kaoss_dest_name(uint8_t dst);   // 3-letter mnemonic
    bool kaoss_active_  = false;    // finger currently on the field
    bool kaoss_all_     = false;    // false = track under cursor, true = ALL 8 tracks
    bool stick_sync_    = true;     // left stick drives kaoss dests; false = stick inert
                                    // (center-of-stick != neutral for unipolar dests like DSM,
                                    // so the user can unplug the stick entirely)
    int  kaoss_x_ = 500;            // 0..1000 normalized field position
    int  kaoss_y_ = 500;
    int  kaoss_track_ = 0;          // track grabbed by the current/last gesture
    // baseline snapshot of the grabbed track's params: captured on gesture
    // start, restored by a short ramp on release (no stuck closed filters).
    struct KaossBase {
        fx::q15 cutoff, resonance, send_del, send_rev, mg_rate, mix_vol, pan;
        int16_t mg_to_cutoff, mg_to_vca;
        uint8_t bits, downsample;
    };
    // per-track baselines + a mask of which tracks the gesture grabbed
    // (one bit in single mode, 0xFF in ALL mode)
    KaossBase kaoss_base_[8]{};
    uint8_t kaoss_grab_mask_ = 0;
    int  kaoss_release_ = 0;        // frames left of the release ramp (0 = idle)
    static constexpr int KAOSS_REL_FRAMES = 20;   // ~330ms @60fps
    // finger trail (ring buffer of recent px positions, drawn fading)
    static constexpr int KAOSS_TRAIL = 20;
    int16_t kaoss_trail_x_[KAOSS_TRAIL] = {0};
    int16_t kaoss_trail_y_[KAOSS_TRAIL] = {0};
    uint8_t kaoss_trail_len_ = 0;   // valid entries
    uint8_t kaoss_trail_pos_ = 0;   // next write index
    // kaoss methods (core/ui/screens/kaoss.cpp)
    void kaoss_touch(int x, int y, bool is_move);  // buttons + menu + field hit
    void draw_kaoss(Draw& d);                      // field + crosshair + trail + menu
    void apply_kaoss();                            // XY -> mixer track params
    void apply_kaoss_dest(int trk, uint8_t dest, int v);  // one destination <- 0..1000
    void kaoss_tick();                             // release ramp (from tick())
    void kaoss_snapshot(int trk);                  // capture baseline for one track

    // === live stick modulation baseline (same contract as kaoss) ===
    // sticks used to write cutoff/sends/bits and leave them there - stuck FX
    // after any stick twitch. now: snapshot on deflect, ramp back on release.
    // LEFT stick is SYNCED to the kaoss pad: it drives the same assigned X/Y
    // destinations and honors the TRK/ALL target toggle. RIGHT stick keeps the
    // fixed pair (delay+reverb send / bitcrush) - four performance axes total.
    KaossBase stick_bases_[8]{};
    uint8_t stick_mask_    = 0;
    int  stick_track_   = 0;
    bool stick_was_on_  = false;
    int  stick_release_ = 0;

public:
    // audio underrun counter (worker starved - written by platform each frame,
    // shown in the fullscreen scope footer for on-device diagnosis)
    uint32_t debug_xruns = 0;
private:

    // bottom-screen panel selector for the Instrument view (Sampler type).
    // KB = touch keyboard; WAVE = waveform + destructive ops (trim/norm/rev/fade);
    // SLICE = chop editor; LOAD = wav browser; REC = master bounce into the slot.
    // (m8-style: the sample editor lives inside the instrument, not a separate screen)
    enum class InstPanel : uint8_t { Kb = 0, Wave, Slice, Load, Rec };
    InstPanel inst_panel_ = InstPanel::Kb;
    // DrumKit bottom panel: KB or GEN (procedural drum generator onto the pad
    // under the cursor). separate from inst_panel_ - different tab row.
    enum class KitPanel : uint8_t { Kb = 0, Gen };
    KitPanel kit_panel_ = KitPanel::Kb;
    // GEN panel: generate a drum into the given pad's slot (allocates a free
    // bank slot for empty pads). returns the slot used or -1.
    int gen_drum_to_pad(int pad, int drum_type);
    void draw_gen_panel(Draw& d);
    bool kit_tab_touch(int x, int y);
    void draw_kit_tabs(Draw& d);
    bool gen_panel_touch(int x, int y);

    bool    save_request_ = false;
    bool    load_request_ = false;
    bool    render_request_ = false;
    bool    reset_request_ = false;
    bool    rec_mode_ = false;       // live record mode toggle
    bool    recording_now_ = false;  // audio capture live (mic/resample) - red tint
    bool    rec_tint_on_ = false;    // tint currently applied (needs restore)

public:
    // flags for main re: project menu (cur_slot and action)
    int  proj_action_slot = -1;       // slot for the action (-1 = none)
    enum class ProjAction { None, Load, Save, New, Delete } proj_action = ProjAction::None;
    bool consume_proj_action(int& slot, ProjAction& a) {
        if (proj_action == ProjAction::None) return false;
        slot = proj_action_slot;
        a = proj_action;
        proj_action = ProjAction::None;
        proj_action_slot = -1;
        return true;
    }
    // main sets this when scanning the sd - true if the slot file exists
    bool slot_present[16] = {false};
    char slot_names[16][24] = {{0}};   // project name per saved slot (peeked from file)
    char slot_status[64] = {0};   // status text at the bottom ("loaded slot 03" etc)
    uint32_t slot_status_frame_ = 0;   // when the status was set (fade-out anim)
    int  proj_confirm_delete_ = -1;   // slot awaiting delete confirmation (-1 = none)
    int  proj_confirm_load_   = -1;   // slot awaiting load-over-unsaved confirmation (-1 = none)

    // === fullscreen scope (performance visualizer) ===
    // toggled from main via held_L + SELECT. when on - the top screen is entirely an oscilloscope
    bool scope_full = false;
    void toggle_scope_full() { scope_full = !scope_full; }

    // === theme (runtime palette switch; persisted by main in a config file) ===
    int theme_idx = 0;
    // theme picker overlay (bottom screen): opened by tapping the wordmark
    bool theme_menu_ = false;
    uint32_t theme_menu_frame_ = 0;
    void draw_theme_menu(Draw& d);          // overlay panel with swatches
    bool theme_menu_touch(int x, int y);    // true = touch consumed
    void cycle_theme() {
        theme_idx = (theme_idx + 1) % pal::theme_count();
        pal::apply_theme(theme_idx);
        mark_dirty();
    }
    void set_theme(int i) {
        if (i < 0 || i >= pal::theme_count()) i = 0;
        theme_idx = i;
        pal::apply_theme(i);
    }
    // recording tint: main tells us if audio capture is live (mic / resample);
    // tick() re-applies the theme + breathing red tint while it lasts.
    void set_recording(bool r) { recording_now_ = r; }

    // dirty flag - true if the user changed something. used in main to decide whether autosave is needed
    bool dirty = false;
    void mark_dirty() { dirty = true; }

    // === system status (set by platform/main every frame) ===
    // battery_level: 0..5 (ptmu), charging: on charger, hour/minute: RTC
    int  battery_level = -1;     // -1 = unknown
    bool battery_charging = false;
    int  clock_hour = -1;
    int  clock_min  = 0;
    void set_system_status(int batt, bool charging, int hour, int minute) {
        battery_level = batt; battery_charging = charging;
        clock_hour = hour; clock_min = minute;
    }
private:
};

} // namespace trackr::ui
