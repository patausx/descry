// App: sequencer and instrument history snapshot/commit/undo glue.
#include "../app.h"

namespace trackr::ui {

// === undo/redo glue ===
// snapshot the step under the cursor BEFORE a mutation; commit records before+after.
void App::snapshot_step(int row) {
    seq::UndoStack::read_cell(project_, seq::EditKind::Step, cur_phrase_, (uint16_t)row, step_before_);
    step_snap_taken_ = true;
}

void App::commit_step(int row) {
    if (!step_snap_taken_) return;
    step_snap_taken_ = false;
    seq::EditRecord::Payload after;
    if (!seq::UndoStack::read_cell(project_, seq::EditKind::Step, cur_phrase_, (uint16_t)row, after)) return;
    const bool changed = std::memcmp(&step_before_, &after, sizeof(after)) != 0;
    // coalesce consecutive edits of the same cell within ~30 frames (~0.5s at 60fps)
    undo_.record(seq::EditKind::Step, cur_phrase_, (uint16_t)row, step_before_, after, frame_, 30);
    if (changed) mark_project_dirty();
}

// snapshot/commit for the CURRENT instrument. same before/after discipline as
// steps, but the payload is the whole Instrument - the only way to make preset
// loads and type switches reversible.
void App::snapshot_inst() {
    inst_undo_.snapshot(project_, cur_inst_);
}

void App::commit_inst() {
    if (inst_undo_.commit(project_, cur_inst_, frame_, 30)) mark_project_dirty();
}

void App::do_undo() {
    // in the instrument view ZL+B walks the INSTRUMENT history - the user is
    // looking at instrument params, so that is what they expect to come back.
    if (screen_ == Screen::Instrument) {
        uint16_t id;
        if (inst_undo_.undo(project_, id)) {
            cur_inst_ = (uint8_t)id;
            mark_project_dirty();
            edit_flash_frame_ = frame_;
            inst_motion_kind_ = 3;
            inst_motion_frame_ = frame_;
            push_live_inst_params(cur_inst_);
            return;
        }
        // nothing in the instrument history - fall through to the cell history
    }
    seq::EditKind k; uint16_t a, b;
    uint16_t step_mask = 0;
    if (!undo_.undo(project_, k, a, b, &step_mask)) return;
    mark_project_dirty();
    edit_flash_frame_ = frame_;
    // move the user to where the change happened so the undo is visible
    if (k == seq::EditKind::Step) {
        cur_phrase_ = (uint8_t)a;
        cursor_row_ = (int)b;
        screen_ = Screen::Phrase;
        start_phrase_motion(step_mask, -1);
    }
}

void App::do_redo() {
    if (screen_ == Screen::Instrument) {
        uint16_t id;
        if (inst_undo_.redo(project_, id)) {
            cur_inst_ = (uint8_t)id;
            mark_project_dirty();
            edit_flash_frame_ = frame_;
            inst_motion_kind_ = 3;
            inst_motion_frame_ = frame_;
            push_live_inst_params(cur_inst_);
            return;
        }
    }
    seq::EditKind k; uint16_t a, b;
    uint16_t step_mask = 0;
    if (!undo_.redo(project_, k, a, b, &step_mask)) return;
    mark_project_dirty();
    edit_flash_frame_ = frame_;
    if (k == seq::EditKind::Step) {
        cur_phrase_ = (uint8_t)a;
        cursor_row_ = (int)b;
        screen_ = Screen::Phrase;
        start_phrase_motion(step_mask, 1);
    }
}

} // namespace trackr::ui
