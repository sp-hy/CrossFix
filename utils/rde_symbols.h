#pragma once
#include <cstdint>

// Offsets from the module base of CHRONOCROSS.exe v1.0.1.0 (image base
// 0x400000), named after the OpenCross symbol list. A name ending in
// _plus_<hex> is a patched site inside that function: a code address, or the
// address of the absolute operand of the instruction there. Names marked
// "proposed" are not in the symbol list yet.
namespace rde {

constexpr uintptr_t image_base = 0x400000;
constexpr uintptr_t guest_origin = 0x80000000;

constexpr uintptr_t Rva(uintptr_t va) { return va - image_base; }

constexpr uintptr_t guest_ram_base = Rva(0x00A2F28C);

constexpr uintptr_t Guest(uintptr_t guestAddress) {
  return guest_ram_base + (guestAddress - guest_origin);
}

inline void *GuestToHost(uintptr_t moduleBase, uintptr_t guestAddress) {
  return reinterpret_cast<void *>(moduleBase + Guest(guestAddress));
}

// pause
constexpr uintptr_t sys_window_proc = Rva(0x00584010);
// jne (0F 85) that pauses the game when the window loses focus
constexpr uintptr_t sys_window_proc_plus_86 = sys_window_proc + 0x86;

// render and screen config
constexpr uintptr_t render_size_w = Rva(0x0122F440);
constexpr uintptr_t render_size_h = Rva(0x0122F444);
constexpr uintptr_t render_overlay_rows = Rva(0x0122F448);
constexpr uintptr_t render_crop_band = Rva(0x0122F44C);
// proposed: config copy of the resolution width
constexpr uintptr_t render_screen_width = Rva(0x0122F488);
// proposed: config copy of the resolution height
constexpr uintptr_t render_screen_height = Rva(0x0122F48C);
// proposed: config copy of ScreenType (0 and 1 are the two stretch modes)
constexpr uintptr_t render_screen_type = Rva(0x0122F494);

// widescreen: software GTE perspective transform
// proposed: add of OFX to the screen X in the first transform routine
constexpr uintptr_t gte_ofx_add_a = Rva(0x0058AD25);
// proposed: the same add in the second transform routine
constexpr uintptr_t gte_ofx_add_b = Rva(0x0058EE7B);

// field movies
constexpr uintptr_t fld_movie_active = Guest(0x800EB760);

// game state
constexpr uintptr_t name_game_mode = Rva(0x00AA1389);
constexpr uintptr_t sys_game_paused = Rva(0x01CB2C5D);
// proposed: guest byte that is 1 while the in-game menu is open (a stack
// resident flag of the main loop, meaning taken from observed behaviour)
constexpr uintptr_t menu_open_flag = Guest(0x801FFE94);

// message windows
constexpr uintptr_t msg_window_place = Rva(0x00439840);
// mov eax,[edi+width]: widest layout line of the window
constexpr uintptr_t msg_window_place_plus_328 = msg_window_place + 0x328;
// mov eax,[edi+40h]: cursor x of the window page
constexpr uintptr_t msg_window_place_plus_391 = msg_window_place + 0x391;

constexpr uintptr_t msg_frame_build_pieces = Rva(0x0043FF90);
// movd xmm1,[render_size_h] operand: portrait width unit
constexpr uintptr_t msg_frame_build_pieces_plus_162d =
    msg_frame_build_pieces + 0x162D;
// imm32 70.0f stored as the window cursor width
constexpr uintptr_t msg_frame_build_pieces_plus_1668 =
    msg_frame_build_pieces + 0x1668;

constexpr uintptr_t msg_layout_glyphs = Rva(0x00444540);
// mulss xmm0,[0.35] operand
constexpr uintptr_t msg_layout_glyphs_plus_69d = msg_layout_glyphs + 0x69D;
// mulss xmm2,[0.45] operand: per glyph advance of the line width
constexpr uintptr_t msg_layout_glyphs_plus_7d5 = msg_layout_glyphs + 0x7D5;

// proposed: restores a message window's layout state (width, cursor x) from a
// saved copy
constexpr uintptr_t msg_window_state_restore = Rva(0x00451F00);
// mov eax,[ecx+esi+40h]: saved cursor x copied into the window
constexpr uintptr_t msg_window_state_restore_plus_3b0 =
    msg_window_state_restore + 0x3B0;

// proposed: builds the four vertices of a sprite quad (unit corners times the
// size, rotated and offset)
constexpr uintptr_t hd_sprite_quad_build = Rva(0x00595AE0);
// mulps xmm1,xmm4: corner times the quad size
constexpr uintptr_t hd_sprite_quad_build_plus_1f6 = hd_sprite_quad_build + 0x1F6;

// battle and menu UI
constexpr uintptr_t pc_text_queue_draw = Rva(0x00422110);
// movd xmm0,[render_size_w] operand
constexpr uintptr_t pc_text_queue_draw_plus_4c = pc_text_queue_draw + 0x4C;
// subss xmm3,[60.0] operand: post-battle stat name offset
constexpr uintptr_t pc_text_queue_draw_plus_428 = pc_text_queue_draw + 0x428;

// proposed: draws the queued PC text runs of one layer, placing each run from
// the font style table
constexpr uintptr_t pc_text_queue_layer_draw = Rva(0x0041D1C0);
// movd xmm4,[render_size_w] operand
constexpr uintptr_t pc_text_queue_layer_draw_plus_60 =
    pc_text_queue_layer_draw + 0x60;
// movss xmm1,[ebp-90h]: pen x of a run
constexpr uintptr_t pc_text_queue_layer_draw_plus_6ff =
    pc_text_queue_layer_draw + 0x6FF;

constexpr uintptr_t hook_font_text_render_800c0228_obs = Rva(0x0041C840);
// add [font_text_pen_x],eax: advances the pen after a glyph run
constexpr uintptr_t hook_font_text_render_800c0228_obs_plus_2bc =
    hook_font_text_render_800c0228_obs + 0x2BC;
// proposed: x of the text pen of the run being laid out
constexpr uintptr_t font_text_pen_x = Rva(0x01C6DCB0);

constexpr uintptr_t hook_hd_overlay_sprite_800c2f88_rep = Rva(0x0041CEA0);
// movups [eax],xmm0 then mov [eax+10h],0: store of a UI element rectangle
constexpr uintptr_t hook_hd_overlay_sprite_800c2f88_rep_plus_88 =
    hook_hd_overlay_sprite_800c2f88_rep + 0x88;

// proposed: draws the title image with its strip quads, fading by an alpha byte
constexpr uintptr_t hd_title_image_draw = Rva(0x004263B0);
// movd xmm2,[render_size_w] operand
constexpr uintptr_t hd_title_image_draw_plus_28 = hd_title_image_draw + 0x28;
// jne (75 0A) over the movss that loads the image x offset
constexpr uintptr_t hd_title_image_draw_plus_9e = hd_title_image_draw + 0x9E;
// movss xmm1,[-7.0] operand: image x offset
constexpr uintptr_t hd_title_image_draw_plus_a4 = hd_title_image_draw + 0xA4;

// proposed: draws the HD overlay sprite list (position, size and sprite id
// records)
constexpr uintptr_t hd_overlay_sprites_draw = Rva(0x00428270);
// movd xmm1,[render_size_w] operand
constexpr uintptr_t hd_overlay_sprites_draw_plus_38 =
    hd_overlay_sprites_draw + 0x38;

constexpr uintptr_t hd_world_name_draw = Rva(0x004290C0);
// movd xmm2,[render_size_w] operand: menu line quad width
constexpr uintptr_t hd_world_name_draw_plus_255 = hd_world_name_draw + 0x255;

// proposed: draws the rotating compass hand sprite of the main menu
constexpr uintptr_t hd_menu_compass_hand_draw = Rva(0x004294B0);
// movd xmm1,[render_size_w] operand
constexpr uintptr_t hd_menu_compass_hand_draw_plus_38 =
    hd_menu_compass_hand_draw + 0x38;

// proposed: draws the underline strips of the menu lists and the battle
// element rows
constexpr uintptr_t hd_menu_underlines_draw = Rva(0x00429850);
// movss xmm2,[735.0] operand
constexpr uintptr_t hd_menu_underlines_draw_plus_5c =
    hd_menu_underlines_draw + 0x5C;
// movss xmm3,[23.0] operand
constexpr uintptr_t hd_menu_underlines_draw_plus_8c =
    hd_menu_underlines_draw + 0x8C;
// movss xmm0,[ebx-18h]
constexpr uintptr_t hd_menu_underlines_draw_plus_193 =
    hd_menu_underlines_draw + 0x193;
// addss xmm0,[31.5] operand
constexpr uintptr_t hd_menu_underlines_draw_plus_307 =
    hd_menu_underlines_draw + 0x307;
// movss xmm2,[852.0]
constexpr uintptr_t hd_menu_underlines_draw_plus_3b7 =
    hd_menu_underlines_draw + 0x3B7;
// mulss xmm0,[33.5] operand
constexpr uintptr_t hd_menu_underlines_draw_plus_3d8 =
    hd_menu_underlines_draw + 0x3D8;
// addss xmm0,[24.0] operand
constexpr uintptr_t hd_menu_underlines_draw_plus_428 =
    hd_menu_underlines_draw + 0x428;
// movss xmm0,[edi-0Ch]
constexpr uintptr_t hd_menu_underlines_draw_plus_4e4 =
    hd_menu_underlines_draw + 0x4E4;
// movss xmm0,[esp+20h]
constexpr uintptr_t hd_menu_underlines_draw_plus_581 =
    hd_menu_underlines_draw + 0x581;
// movss xmm2,[735.0] operand
constexpr uintptr_t hd_menu_underlines_draw_plus_65b =
    hd_menu_underlines_draw + 0x65B;
// mulss xmm0,[21.0] operand
constexpr uintptr_t hd_menu_underlines_draw_plus_66d =
    hd_menu_underlines_draw + 0x66D;
// movss xmm0,[edi-18h]
constexpr uintptr_t hd_menu_underlines_draw_plus_77f =
    hd_menu_underlines_draw + 0x77F;
// addss xmm0,[31.5] operand
constexpr uintptr_t hd_menu_underlines_draw_plus_7d0 =
    hd_menu_underlines_draw + 0x7D0;

constexpr uintptr_t hd_draw_pause_menu = Rva(0x005AD280);
// movd xmm0,[render_size_w] operand: width of the menu container
constexpr uintptr_t hd_draw_pause_menu_plus_27e = hd_draw_pause_menu + 0x27E;

// proposed: float constants in the read-only data
constexpr uintptr_t rdata_f32_0_35 = Rva(0x006CB8A4);
constexpr uintptr_t rdata_f32_0_45 = Rva(0x006CB8E4);
constexpr uintptr_t rdata_f32_21 = Rva(0x006CBABC);
constexpr uintptr_t rdata_f32_23 = Rva(0x006CBAC8);
constexpr uintptr_t rdata_f32_24 = Rva(0x006CBACC);
constexpr uintptr_t rdata_f32_31_5 = Rva(0x006CBAEC);
constexpr uintptr_t rdata_f32_33_5 = Rva(0x006CBB00);
constexpr uintptr_t rdata_f32_60 = Rva(0x006CBB50);
constexpr uintptr_t rdata_f32_735 = Rva(0x006CBCB8);
constexpr uintptr_t rdata_f32_852 = Rva(0x006CBCD8);
constexpr uintptr_t rdata_f32_minus_7 = Rva(0x006CBDCC);

// save selector
// proposed: C run-time block copy with overlap handling (memcpy shares it)
constexpr uintptr_t crt_memmove = Rva(0x0065FDD0);
// mov edx,[esi]: dword copy loop of the block copy
constexpr uintptr_t crt_memmove_plus_507 = crt_memmove + 0x507;
// proposed: first of four tables of save slot quads in the extended guest RAM
constexpr uintptr_t save_menu_quad_table_a = Guest(0x805EF2D0);
constexpr uintptr_t save_menu_quad_table_b = save_menu_quad_table_a + 0x3E80;
constexpr uintptr_t save_menu_quad_table_c = save_menu_quad_table_a + 0x8C0;
constexpr uintptr_t save_menu_quad_table_d = save_menu_quad_table_a + 0x4740;

// guest hooks and overlays
// void __stdcall register_hook(uint32_t guest_pc, void (*handler)()): binds a
// handler to a guest PC; the interpreter calls it before running the
// instruction there
constexpr uintptr_t register_hook = Rva(0x00588C30);
constexpr uint32_t guest_ram_size = 0x800000;
// guest general registers r0..r31, one word each
constexpr uintptr_t guest_regs = Rva(0x01C25880);
constexpr int guest_reg_s3 = 19;
// ID word of the resident mode overlay (6 = field, 8 = battle, 9 = title)
constexpr uint32_t mode_overlay_id = 0x800A9F00;
constexpr uint32_t mode_overlay_field = 6;
constexpr uint32_t mode_overlay_battle = 8;
// graphics mode read at boot: 1 HD art (chip2 backgrounds are built)
constexpr uintptr_t sys_graphics_hd = Rva(0x0122F4C0);

// widescreen: field camera limits (guest)
// s16 x max, x min, y max, y min
constexpr uint32_t fld_scroll_clamp = 0x800E9FE4;
constexpr uint32_t fld_camera_scroll_apply = 0x800B2BAC;
// first instruction after the room loader has stored the room's limits and
// the y min bias
constexpr uint32_t fld_room_load_limits_done = 0x800ACB20;
// first instruction after fld_set_scroll_limits has stored the four limits
constexpr uint32_t fld_set_scroll_limits_done = 0x800BF1C4;

// fps
// RDE's field pad-latch hook (guest fld_pad_filter_update): copies the fast and
// slow toggles into the field latches, or zeroes them while the input lock
// counts down or the room is 386.
constexpr uintptr_t hook_speed_boost_800add80_obs = Rva(0x00448EF0);
// cmp word [input lock], 0: absolute operand
constexpr uintptr_t hook_speed_boost_800add80_obs_plus_c =
    hook_speed_boost_800add80_obs + 0xC;
// cmp word [map id], ax: absolute operand
constexpr uintptr_t hook_speed_boost_800add80_obs_plus_2a =
    hook_speed_boost_800add80_obs + 0x2A;
// mov byte [fast latch], al: absolute operand
constexpr uintptr_t hook_speed_boost_800add80_obs_plus_47 =
    hook_speed_boost_800add80_obs + 0x47;
// mov byte [slow latch], al: absolute operand
constexpr uintptr_t hook_speed_boost_800add80_obs_plus_51 =
    hook_speed_boost_800add80_obs + 0x51;
// Bytes: fast and slow mode as latched for the current field frame
constexpr uintptr_t sys_field_fast_latch = Rva(0x01CB39B5);
constexpr uintptr_t sys_field_slow_latch = Rva(0x01CB39B4);

// fps: field frame driver (guest)
constexpr int guest_reg_a1 = 5;
// vsync count stored by the main loop at every frame
constexpr uint32_t sys_last_vsync_count = 0x800859B0;
// u16 field frames the pad still reads as released
constexpr uint32_t fld_input_lock_frames = 0x800EC4FA;
// u16 current field map id; RDE keeps its speed modes off in map 0x182 (386)
constexpr uint32_t fld_map_id = 0x800EBAAE;
constexpr uint16_t fld_speed_locked_map = 0x182;
// per field frame: speed gates, tick count, logic ticks, draw, then a wait
// until fld_frame_vsyncs vblanks have passed since the previous frame
constexpr uint32_t fld_frame_driver = 0x800AE354;
// first speed gate: lbu v0, var 0x36 copy (reached every frame)
constexpr uint32_t fld_frame_driver_plus_478 = fld_frame_driver + 0x478;
constexpr uint32_t fld_frame_driver_plus_478_word = 0x92025294;
// move a0, zero: normal pace, reached only when every speed gate passed and
// neither fast nor slow mode took its path
constexpr uint32_t fld_frame_driver_plus_574 = fld_frame_driver + 0x574;
constexpr uint32_t fld_frame_driver_plus_574_word = 0x00002021;
// lh a1, fld_frame_vsyncs: the end-of-frame wait loads the frame period
constexpr uint32_t fld_frame_driver_plus_1020 = fld_frame_driver + 0x1020;
constexpr uint32_t fld_frame_driver_plus_1020_word = 0x86050302;
// the next instruction, with the period in a1
constexpr uint32_t fld_frame_driver_plus_1024 = fld_frame_driver + 0x1024;

// fps: script op CA, face the following walk's goal (guest, field overlay).
// Its first run sets the facing target and the turn request bit and yields
// with the pc unchanged; a later run advances only while the bit is still set.
// fld_step_anim_request_turn takes the request up (clears the bit) once per
// frame, after the frame's logic ticks.
constexpr uint32_t fld_op_face_target_then_advance = 0x800D0930;
// lw v1, 4(v0): the running entity
constexpr uint32_t fld_op_face_target_then_advance_plus_10c =
    fld_op_face_target_then_advance + 0x10C;
constexpr uint32_t fld_op_face_target_then_advance_plus_10c_word = 0x8C430004;
// ori v0, v0, 0x6000: the first run sets the request bits, entity in v1
constexpr uint32_t fld_op_face_target_then_advance_plus_11c =
    fld_op_face_target_then_advance + 0x11C;
constexpr uint32_t fld_op_face_target_then_advance_plus_11c_word = 0x34426000;
constexpr int guest_reg_v1 = 3;
// field entity record (0x278 bytes)
constexpr uint32_t fld_actor_size = 0x278;
// u32 script pc, an offset into the room's script image
constexpr uint32_t fld_actor_script_pc = 0x228;
// u32 state bits; bit 14 is the turn request
constexpr uint32_t fld_actor_state = 0x270;
constexpr uint32_t fld_actor_state_turn_request = 0x4000;

// fps stats: battle frame (guest, battle overlay)
// battle per-frame cycle; its first word is addiu sp, sp, -0x20
constexpr uint32_t btl_frame_cycle = 0x800BDEA0;
constexpr uint32_t btl_frame_cycle_word = 0x27BDFFE0;

// voices: field message windows
// Fields of one msg_windows (0x012BFD80) entry.
constexpr uintptr_t msg_window_room = 0x04;
constexpr uintptr_t msg_window_message = 0x08;
constexpr uintptr_t msg_window_page = 0x18;
constexpr uintptr_t msg_window_in_use = 0x4DCD4;

constexpr uintptr_t msg_window_open = Rva(0x00439690);
// mov ecx,[0x01C299A0]: first instruction after msg_text_open built the text
constexpr uintptr_t msg_window_open_text_ready = msg_window_open + 0x12C;
constexpr int msg_window_open_text_ready_size = 6;

// speaker of the line's generated words: the script's speaker, 0 becoming
// lynx_body while name_lynx_in_party is set (Pip as his current form, -1 for
// two lines RDE keeps without one)
constexpr uintptr_t name_speaker_column = Rva(0x006E3CF8);
// byte: a party member wears model 0x31
constexpr uintptr_t name_lynx_in_party = Rva(0x0075DD52);
constexpr int lynx_body = 0x31;
constexpr uintptr_t msg_room_loaded = Rva(0x006E3CF0);
constexpr uintptr_t msg_room_rows = Rva(0x01C78418);
constexpr uintptr_t msg_room_ex_rows = Rva(0x01C78C18);
constexpr int msg_room_ex_variants = 50;

// voices: battle message window
// RDE's observer of the guest jal btl_msg_setup in btl_op_e0_msg_window_open;
// push ebp; mov ebp,esp; push -1
constexpr uintptr_t hook_font_text_render_800d191c_obs = Rva(0x0041C3A0);
constexpr int hook_font_text_render_800d191c_obs_prologue = 5;

// battle script op E0 (guest, battle overlay), a0 = the running entity,
// a1 = the instruction; its first word is addiu sp, sp, -0x40
constexpr uint32_t btl_op_e0_msg_window_open = 0x800D1890;
constexpr uint32_t btl_op_e0_msg_window_open_word = 0x27BDFFC0;
// jal btl_msg_setup: the window opens (a0 has been overwritten by then)
constexpr uint32_t btl_op_e0_msg_window_open_plus_8c =
    btl_op_e0_msg_window_open + 0x8C;
constexpr uint32_t btl_op_e0_msg_window_open_plus_8c_word = 0x0C02FEF3;
constexpr int guest_reg_a0 = 4;
// pointer to the battle script state; its first words point at the entity
// records, indexed by entity
constexpr uint32_t btl_script_state_ptr = 0x800E2FC8;
// proposed: byte of an entity record, the unit slot the entity is bound to
// (9 for an entity of the scene itself)
constexpr uint32_t btl_entity_unit = 0x83;
// unit slots: 3 PCs, then 6 enemies
constexpr int btl_unit_slots = 9;

// proposed: per unit slot (16) RDE's label record of the unit, filled by
// hook_font_text_render_800c3b10_obs: slot, is enemy, character or monster
// id, enemy letter, present
constexpr uintptr_t name_battle_unit_labels = Rva(0x01C6DCB8);
constexpr int name_battle_unit_label_size = 5;
constexpr uintptr_t hook_font_text_render_800c3b10_obs = Rva(0x0041BD10);
// mov [ecx+ecx*4+name_battle_unit_labels],cl: absolute operand
constexpr uintptr_t hook_font_text_render_800c3b10_obs_plus_1e =
    hook_font_text_render_800c3b10_obs + 0x1E;
// std::string *__thiscall (const label record *, std::string *out, bool
// no_letter): display name of a battle unit in the current language (scene
// 0x186 and 0x1DD names, Lynx's body, the monster or character name), the
// enemy letter appended unless no_letter
constexpr uintptr_t name_battle_unit_label = Rva(0x00418910);
// proposed: C run-time operator delete(void *, size_t), the deallocation of
// the game's std::string buffers
constexpr uintptr_t crt_operator_delete = Rva(0x005E9513);

constexpr uint32_t btl_battle_id = 0x80075906;
constexpr uint32_t btl_window1 = 0x800E00C4;
constexpr uint32_t btl_window1_page = btl_window1 + 0x30;
constexpr uint32_t btl_window_state = 0x800E0200;
constexpr int btl_event_window = 4;
constexpr uint8_t btl_window_state_open = 2;

// precise geometry: GTE
constexpr uintptr_t gte_exec_command = Rva(0x0058A990);
constexpr uintptr_t gte_exec_cop2 = Rva(0x0058F940);
// call gte_exec_command: its only call, taken by every GTE command
constexpr uintptr_t gte_exec_cop2_plus_18 = gte_exec_cop2 + 0x18;
constexpr uintptr_t gte_nclip_fine = Rva(0x0058F770);
// push ebp; mov ebp,esp; and esp,-8; sub esp,20h
constexpr int gte_nclip_fine_prologue = 9;
// proposed: body of the CompMatrix port (ecx = parent, edx = local, one stack
// argument = result, removed by the caller); products through the GTE
constexpr uintptr_t gte_comp_matrix_impl = Rva(0x004FD390);
// sub esp,18h; push ebx; push ebp
constexpr int gte_comp_matrix_impl_prologue = 5;
// guest memory region objects indexed by address >> 19 (vtable +0x0C read32)
constexpr uintptr_t guest_region_table = Rva(0x01C2599C);
// fine-Z recorder: ecx = guest address of the primitive a face builder is
// about to write, edx = its OT entry, two stack arguments (depth, shift) the
// caller removes; called after the face's projections
constexpr uintptr_t hd_record_fine_z = Rva(0x004355F0);
// ports of sys_weapon_draw_gt3/gt4, sys_model_face_ft3_avg, ft3_near,
// ft4_avg, ft4_near, sys_weapon_lit_gt3/gt4, and their call of
// hd_record_fine_z for each face they keep
constexpr uintptr_t hook_native_port_800a067c_rep = Rva(0x0055A120);
constexpr uintptr_t hook_native_port_800a067c_rep_plus_a9f =
    hook_native_port_800a067c_rep + 0xA9F;
constexpr uintptr_t hook_native_port_800a0bc4_rep = Rva(0x0055B710);
constexpr uintptr_t hook_native_port_800a0bc4_rep_plus_b11 =
    hook_native_port_800a0bc4_rep + 0xB11;
constexpr uintptr_t hook_native_port_800a3ce4_rep = Rva(0x00564A70);
constexpr uintptr_t hook_native_port_800a3ce4_rep_plus_777 =
    hook_native_port_800a3ce4_rep + 0x777;
constexpr uintptr_t hook_native_port_800a3f54_rep = Rva(0x00565560);
constexpr uintptr_t hook_native_port_800a3f54_rep_plus_6db =
    hook_native_port_800a3f54_rep + 0x6DB;
constexpr uintptr_t hook_native_port_800a41e8_rep = Rva(0x00565FE0);
constexpr uintptr_t hook_native_port_800a41e8_rep_plus_959 =
    hook_native_port_800a41e8_rep + 0x959;
constexpr uintptr_t hook_native_port_800a44d4_rep = Rva(0x00566D00);
constexpr uintptr_t hook_native_port_800a44d4_rep_plus_834 =
    hook_native_port_800a44d4_rep + 0x834;
constexpr uintptr_t hook_native_port_800a6da4_rep = Rva(0x00571110);
constexpr uintptr_t hook_native_port_800a6da4_rep_plus_97a =
    hook_native_port_800a6da4_rep + 0x97A;
constexpr uintptr_t hook_native_port_800a76d0_rep = Rva(0x005735D0);
constexpr uintptr_t hook_native_port_800a76d0_rep_plus_d79 =
    hook_native_port_800a76d0_rep + 0xD79;

// Offsets in the GTE register block (PS1 register order, one word each)
constexpr uintptr_t gte_rotation = 0x00;
constexpr uintptr_t gte_translation = 0x14;
constexpr uintptr_t gte_ofx = 0x60;
constexpr uintptr_t gte_ofy = 0x64;
constexpr uintptr_t gte_h = 0x68;
// V0, V1, V2: {s16 x, s16 y, s16 z, pad}
constexpr uintptr_t gte_vector0 = 0x80;
// IR0 (weight of GPF/GPL), IR1..IR3
constexpr uintptr_t gte_ir0 = 0xA0;
constexpr uintptr_t gte_ir1 = 0xA4;
constexpr uintptr_t gte_sxy0 = 0xB0;
constexpr uintptr_t gte_mac0 = 0xE0;
constexpr uintptr_t gte_mac1 = 0xE4;
// RDE's ring of the last four projections, {s16 x, s16 y} in 1/8 pixels
constexpr uintptr_t gte_fine_ring = 0x100;
constexpr uintptr_t gte_fine_ring_count = 0x110;

// precise geometry: GP0 transport
constexpr uintptr_t dma_gpu_ot_walk = Rva(0x005A8E50);
// call vec_u32_insert_range: appends one packet's words to the stream
constexpr uintptr_t dma_gpu_ot_walk_plus_ba = dma_gpu_ot_walk + 0xBA;
constexpr uintptr_t vec_u32_insert_range = Rva(0x005A9200);
constexpr uintptr_t gpu_cmd_table = Rva(0x006B4AA0);
constexpr int gpu_cmd_table_polygon = 1;
constexpr int gpu_cmd_table_vram_to_cpu_or_fine = 6;
constexpr int gpu_cmd_table_env_or_fine = 7;
constexpr uintptr_t gpu_cmd_polygon = Rva(0x005CA460);
constexpr uintptr_t gpu_cmd_vram_to_cpu_or_fine = Rva(0x005CAAE0);
constexpr uintptr_t gpu_cmd_env_or_fine = Rva(0x005CAE10);

// precise geometry: vertex emits
constexpr uintptr_t vec3_make = Rva(0x00429490);
constexpr uintptr_t crt_roundf = Rva(0x006717A4);
constexpr uintptr_t gpu_emit_vertex = Rva(0x005CB830);
// call vec3_make: position of an integer vertex
constexpr uintptr_t gpu_emit_vertex_plus_5e = gpu_emit_vertex + 0x5E;
constexpr uintptr_t gpu_emit_vertex_fine = Rva(0x005CBA70);
// call crt_roundf: x of a fine vertex to the 4x grid
constexpr uintptr_t gpu_emit_vertex_fine_plus_4c = gpu_emit_vertex_fine + 0x4C;
// call crt_roundf: y of a fine vertex to the 4x grid
constexpr uintptr_t gpu_emit_vertex_fine_plus_8f = gpu_emit_vertex_fine + 0x8F;
// call vec3_make: position of a fine vertex
constexpr uintptr_t gpu_emit_vertex_fine_plus_ae = gpu_emit_vertex_fine + 0xAE;
// Offsets in the GPU parser object: drawing offset (11-bit x, 11-bit y) and
// the packet xy word of the vertex being emitted
constexpr uintptr_t gpu_draw_offset = 0x440;
constexpr uintptr_t gpu_vertex_xy = 0x480;
// proposed: flushes the vertex batch with its bounding box as the viewport
constexpr uintptr_t gpu_batch_flush = Rva(0x005CB5E0);
// mov eax,[esp+14h]; cvttss2si edi,xmm2: the box's maximum x / y (xmm4 /
// xmm1) are truncated just after
constexpr uintptr_t gpu_batch_flush_plus_176 = gpu_batch_flush + 0x176;

// resolution scale: GPU backend
// VRAM move on the GPU copies, through rectangle-sized temporaries; the
// function ends where gpu_d3d_fill_rect starts
constexpr uintptr_t hd_vram_move_gpu = Rva(0x005C7C30);
constexpr uintptr_t hd_vram_move_gpu_end = Rva(0x005C81D0);
// call dword ptr [eax|ecx + 0xB8]: the six CopySubresourceRegion calls
constexpr uintptr_t hd_vram_move_gpu_plus_1ec = hd_vram_move_gpu + 0x1EC;
constexpr uintptr_t hd_vram_move_gpu_plus_23f = hd_vram_move_gpu + 0x23F;
constexpr uintptr_t hd_vram_move_gpu_plus_3fb = hd_vram_move_gpu + 0x3FB;
constexpr uintptr_t hd_vram_move_gpu_plus_44d = hd_vram_move_gpu + 0x44D;
constexpr uintptr_t hd_vram_move_gpu_plus_4fb = hd_vram_move_gpu + 0x4FB;
constexpr uintptr_t hd_vram_move_gpu_plus_55c = hd_vram_move_gpu + 0x55C;
// proposed: puts back the shaders, render target and viewports saved before a
// VRAM upload, move or fill
constexpr uintptr_t gpu_state_restore = Rva(0x005C9270);
// call dword ptr [ecx + 0xB0]: RSSetViewports of the saved viewports
constexpr uintptr_t gpu_state_restore_plus_4b = gpu_state_restore + 0x4B;
// DXBC of the pixel shader that draws uint16 mirror texels into the RGBA
// VRAM target for CPU uploads
constexpr uintptr_t shader_vram_decode_ps = Rva(0x006CAFD8);
constexpr size_t shader_vram_decode_ps_size = 0x348;

} // namespace rde
