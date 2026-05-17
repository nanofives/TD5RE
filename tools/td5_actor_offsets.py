# tools/td5_actor_offsets.py
# Auto-extracted offset map of the 0x388-byte TD5 actor struct.
# Format: offset_hex -> (name, size_bytes, dtype, source_ref)
# dtype: u8 u16 u32 i8 i16 i32 f32 bytes
#
# Sources:
#   - td5_trace.h (TD5_TraceRotationRow comments with verified offsets)
#   - td5_ai.c (ACTOR_* macros with semantic descriptions)
#   - re/include/td5_actor_struct.h (canonical struct definition with offsetof checks)

ACTOR_OFFSETS = {
    # Track Probe States (0x000-0x07F): 8 x 16-byte entries
    # Body probes FL,FR,RL,RR at +0x00, wheel probes FL,FR,RL,RR at +0x40
    # TD5_TrackProbe layout (16 bytes per probe), 4 probes total:
    #   0x00 i16 span_index   0x02 i16 span_normalized
    #   0x04 i16 span_accumulated   0x06 i16 span_high_water
    #   0x08 i16 contact_vertex_a   0x0A i16 contact_vertex_b
    #   0x0C i8  sub_lane_index     0x0D u8 pad   0x0E i16 pad
    0x000: ("body_probe_0_span_raw", 2, "i16", "td5_actor_struct.h:215"),
    0x002: ("body_probe_0_span_norm", 2, "i16", "td5_actor_struct.h:215"),
    0x004: ("body_probe_0_span_accumulated", 2, "i16", "td5_types.h:404"),
    0x006: ("body_probe_0_span_high_water", 2, "i16", "td5_types.h:405"),
    0x010: ("body_probe_1_span_raw", 2, "i16", "td5_types.h:402"),
    0x012: ("body_probe_1_span_norm", 2, "i16", "td5_types.h:403"),
    0x014: ("body_probe_1_span_accumulated", 2, "i16", "td5_types.h:404"),
    0x016: ("body_probe_1_span_high_water", 2, "i16", "td5_types.h:405"),
    0x020: ("body_probe_2_span_raw", 2, "i16", "td5_types.h:402"),
    0x022: ("body_probe_2_span_norm", 2, "i16", "td5_types.h:403"),
    0x024: ("body_probe_2_span_accumulated", 2, "i16", "td5_types.h:404"),
    0x026: ("body_probe_2_span_high_water", 2, "i16", "td5_types.h:405"),
    0x030: ("body_probe_3_span_raw", 2, "i16", "td5_types.h:402"),
    0x032: ("body_probe_3_span_norm", 2, "i16", "td5_types.h:403"),
    0x034: ("body_probe_3_span_accumulated", 2, "i16", "td5_types.h:404"),
    0x036: ("body_probe_3_span_high_water", 2, "i16", "td5_types.h:405"),
    0x040: ("wheel_probe_0_span_raw", 2, "i16", "td5_actor_struct.h:216"),
    0x042: ("wheel_probe_0_span_norm", 2, "i16", "td5_actor_struct.h:216"),
    
    # Track Position State (0x080-0x08F)
    0x080: ("track_span_raw", 2, "i16", "td5_ai.c:99 / td5_actor_struct.h:224"),
    0x082: ("track_span_normalized", 2, "i16", "td5_ai.c:100 / td5_actor_struct.h:225"),
    0x084: ("track_span_accumulated", 2, "i16", "td5_ai.c:101 / td5_actor_struct.h:226"),
    0x086: ("track_span_high_water", 2, "i16", "td5_actor_struct.h:227"),
    0x08C: ("track_sub_lane_index", 1, "u8", "td5_ai.c:102 / td5_actor_struct.h:230"),
    
    # Contact Probe Positions (0x090-0x0BF): wheel positions X,Y,Z
    0x090: ("probe_FL_x", 4, "i32", "td5_actor_struct.h:241"),
    0x094: ("probe_FL_y", 4, "i32", "td5_actor_struct.h:241"),
    0x098: ("probe_FL_z", 4, "i32", "td5_actor_struct.h:241"),
    0x09C: ("probe_FR_x", 4, "i32", "td5_actor_struct.h:242"),
    0x0A0: ("probe_FR_y", 4, "i32", "td5_actor_struct.h:242"),
    0x0A4: ("probe_FR_z", 4, "i32", "td5_actor_struct.h:242"),
    0x0A8: ("probe_RL_x", 4, "i32", "td5_actor_struct.h:243"),
    0x0AC: ("probe_RL_y", 4, "i32", "td5_actor_struct.h:243"),
    0x0B0: ("probe_RL_z", 4, "i32", "td5_actor_struct.h:243"),
    0x0B4: ("probe_RR_x", 4, "i32", "td5_actor_struct.h:244"),
    0x0B8: ("probe_RR_y", 4, "i32", "td5_actor_struct.h:244"),
    0x0BC: ("probe_RR_z", 4, "i32", "td5_actor_struct.h:244"),
    
    # Upper Bounding Box Vertices (0x0C0-0x0EF): body corner positions
    0x0C0: ("bbox_upper_0_x", 4, "i32", "td5_actor_struct.h:255"),
    0x0C4: ("bbox_upper_0_y", 4, "i32", "td5_actor_struct.h:255"),
    0x0C8: ("bbox_upper_0_z", 4, "i32", "td5_actor_struct.h:255"),
    0x0CC: ("bbox_upper_1_x", 4, "i32", "td5_actor_struct.h:255"),
    0x0D0: ("bbox_upper_1_y", 4, "i32", "td5_actor_struct.h:255"),
    0x0D4: ("bbox_upper_1_z", 4, "i32", "td5_actor_struct.h:255"),
    0x0D8: ("bbox_upper_2_x", 4, "i32", "td5_actor_struct.h:255"),
    0x0DC: ("bbox_upper_2_y", 4, "i32", "td5_actor_struct.h:255"),
    0x0E0: ("bbox_upper_2_z", 4, "i32", "td5_actor_struct.h:255"),
    0x0E4: ("bbox_upper_3_x", 4, "i32", "td5_actor_struct.h:255"),
    0x0E8: ("bbox_upper_3_y", 4, "i32", "td5_actor_struct.h:255"),
    0x0EC: ("bbox_upper_3_z", 4, "i32", "td5_actor_struct.h:255"),
    
    # Wheel Contact Positions (0x0F0-0x11F)
    0x0F0: ("wheel_contact_FL_x", 4, "i32", "td5_actor_struct.h:267"),
    0x0F4: ("wheel_contact_FL_y", 4, "i32", "td5_actor_struct.h:267"),
    0x0F8: ("wheel_contact_FL_z", 4, "i32", "td5_actor_struct.h:267"),
    0x0FC: ("wheel_contact_FR_x", 4, "i32", "td5_actor_struct.h:267"),
    0x100: ("wheel_contact_FR_y", 4, "i32", "td5_actor_struct.h:267"),
    0x104: ("wheel_contact_FR_z", 4, "i32", "td5_actor_struct.h:267"),
    0x108: ("wheel_contact_RL_x", 4, "i32", "td5_actor_struct.h:267"),
    0x10C: ("wheel_contact_RL_y", 4, "i32", "td5_actor_struct.h:267"),
    0x110: ("wheel_contact_RL_z", 4, "i32", "td5_actor_struct.h:267"),
    0x114: ("wheel_contact_RR_x", 4, "i32", "td5_actor_struct.h:267"),
    0x118: ("wheel_contact_RR_y", 4, "i32", "td5_actor_struct.h:267"),
    0x11C: ("wheel_contact_RR_z", 4, "i32", "td5_actor_struct.h:267"),
    
    # Main Rotation Matrix (0x120-0x143): 3x3 float
    0x120: ("rotation_matrix", 36, "bytes", "td5_actor_struct.h:276"),
    
    # Render Position (0x144-0x14F): float[3]
    0x144: ("render_pos_x", 4, "f32", "td5_actor_struct.h:286"),
    0x148: ("render_pos_y", 4, "f32", "td5_actor_struct.h:286"),
    0x14C: ("render_pos_z", 4, "f32", "td5_actor_struct.h:286"),
    
    # Saved Orientation (0x150-0x173): float[9]
    0x150: ("saved_orientation", 36, "bytes", "td5_actor_struct.h:294"),
    
    # Collision Spin Matrix (0x180-0x1A3): float[9]
    0x180: ("collision_spin_matrix", 36, "bytes", "td5_actor_struct.h:311"),
    
    # Car Definition & Tuning Pointers (0x1B0-0x1BF)
    0x1B0: ("car_config_ptr", 4, "u32", "td5_actor_struct.h:321"),
    0x1B8: ("car_definition_ptr", 4, "u32", "td5_ai.c:103 / td5_actor_struct.h:323"),
    0x1BC: ("tuning_data_ptr", 4, "u32", "td5_actor_struct.h:326"),
    
    # Velocities & Angular Rates (0x1C0-0x1D7)
    0x1C0: ("angular_velocity_roll", 4, "i32", "td5_trace.h:155 / td5_actor_struct.h:338"),
    0x1C4: ("angular_velocity_yaw", 4, "i32", "td5_trace.h:155 / td5_actor_struct.h:339"),
    0x1C8: ("angular_velocity_pitch", 4, "i32", "td5_trace.h:155 / td5_actor_struct.h:340"),
    0x1CC: ("linear_velocity_x", 4, "i32", "td5_ai.c:109 / td5_actor_struct.h:341"),
    0x1D0: ("linear_velocity_y", 4, "i32", "td5_actor_struct.h:344"),
    0x1D4: ("linear_velocity_z", 4, "i32", "td5_ai.c:110 / td5_actor_struct.h:346"),
    
    # Euler Angle Accumulators (0x1F0-0x1FB)
    0x1F0: ("euler_accum_roll", 4, "i32", "td5_trace.h:156 / td5_actor_struct.h:365"),
    0x1F4: ("euler_accum_yaw", 4, "i32", "td5_ai.c:104 / td5_trace.h:156 / td5_actor_struct.h:365"),
    0x1F8: ("euler_accum_pitch", 4, "i32", "td5_trace.h:156 / td5_actor_struct.h:365"),
    
    # World Position (0x1FC-0x207)
    0x1FC: ("world_pos_x", 4, "i32", "td5_ai.c:111 / td5_actor_struct.h:377"),
    0x200: ("world_pos_y", 4, "i32", "td5_actor_struct.h:377"),
    0x204: ("world_pos_z", 4, "i32", "td5_ai.c:112 / td5_actor_struct.h:377"),
    
    # Display Euler Angles (0x208-0x20D)
    0x208: ("display_angle_roll", 2, "i16", "td5_trace.h:157 / td5_actor_struct.h:388"),
    0x20A: ("display_angle_yaw", 2, "i16", "td5_trace.h:157 / td5_actor_struct.h:388"),
    0x20C: ("display_angle_pitch", 2, "i16", "td5_trace.h:157 / td5_actor_struct.h:388"),
    
    # Wheel Display Angles (0x210-0x22F)
    0x210: ("wheel_disp_angles", 32, "bytes", "td5_actor_struct.h:397"),
    
    # Wheel Contact Normals (0x230-0x24F)
    0x230: ("wheel_contact_normals", 32, "bytes", "td5_actor_struct.h:398"),
    
    # Wheel Contact Velocities (0x250-0x26F)
    0x250: ("wheel_contact_velocities", 32, "bytes", "td5_actor_struct.h:399"),

    # Wheel Contact Deltas (0x270-0x28F) -- per-wheel position deltas.
    # 4 wheels x 8 bytes each: 3 x int16 (x/y/z) + 1 x int16 padding.
    # Writer: RefreshVehicleWheelContactFrames @ 0x00403720 (port td5_physics.c:5459).
    # Reader: UpdateVehicleSuspensionResponse @ 0x004057F0 (port td5_physics.c:3709)
    # uses as spring_dot = sum(delta * wcv) for landing-impulse compute,
    # gated on prev_air & bit. Identified by Agent I 2026-05-16.
    0x270: ("wheel_contact_delta_FL_x", 2, "i16", "agent_I_2026-05-16"),
    0x272: ("wheel_contact_delta_FL_y", 2, "i16", "agent_I_2026-05-16"),
    0x274: ("wheel_contact_delta_FL_z", 2, "i16", "agent_I_2026-05-16"),
    0x276: ("wheel_contact_delta_FL_pad", 2, "i16", "agent_I_2026-05-16"),
    0x278: ("wheel_contact_delta_FR_x", 2, "i16", "agent_I_2026-05-16"),
    0x27A: ("wheel_contact_delta_FR_y", 2, "i16", "agent_I_2026-05-16"),
    0x27C: ("wheel_contact_delta_FR_z", 2, "i16", "agent_I_2026-05-16"),
    0x27E: ("wheel_contact_delta_FR_pad", 2, "i16", "agent_I_2026-05-16"),
    0x280: ("wheel_contact_delta_RL_x", 2, "i16", "agent_I_2026-05-16"),
    0x282: ("wheel_contact_delta_RL_y", 2, "i16", "agent_I_2026-05-16"),
    0x284: ("wheel_contact_delta_RL_z", 2, "i16", "agent_I_2026-05-16"),
    0x286: ("wheel_contact_delta_RL_pad", 2, "i16", "agent_I_2026-05-16"),
    0x288: ("wheel_contact_delta_RR_x", 2, "i16", "agent_I_2026-05-16"),
    0x28A: ("wheel_contact_delta_RR_y", 2, "i16", "agent_I_2026-05-16"),
    0x28C: ("wheel_contact_delta_RR_z", 2, "i16", "agent_I_2026-05-16"),
    0x28E: ("wheel_contact_delta_RR_pad", 2, "i16", "agent_I_2026-05-16"),

    # Heading Normal (0x290-0x295)
    0x290: ("heading_normal_x", 2, "i16", "td5_actor_struct.h:408"),
    0x292: ("heading_normal_y", 2, "i16", "td5_actor_struct.h:408"),
    0x294: ("heading_normal_z", 2, "i16", "td5_actor_struct.h:408"),
    
    # High-Res Wheel World Positions (0x298-0x2C7)
    0x298: ("wheel_world_hires_FL_x", 4, "i32", "td5_actor_struct.h:418"),
    0x29C: ("wheel_world_hires_FL_y", 4, "i32", "td5_actor_struct.h:418"),
    0x2A0: ("wheel_world_hires_FL_z", 4, "i32", "td5_actor_struct.h:418"),
    0x2A4: ("wheel_world_hires_FR_x", 4, "i32", "td5_actor_struct.h:418"),
    0x2A8: ("wheel_world_hires_FR_y", 4, "i32", "td5_actor_struct.h:418"),
    0x2AC: ("wheel_world_hires_FR_z", 4, "i32", "td5_actor_struct.h:418"),
    0x2B0: ("wheel_world_hires_RL_x", 4, "i32", "td5_actor_struct.h:418"),
    0x2B4: ("wheel_world_hires_RL_y", 4, "i32", "td5_actor_struct.h:418"),
    0x2B8: ("wheel_world_hires_RL_z", 4, "i32", "td5_actor_struct.h:418"),
    0x2BC: ("wheel_world_hires_RR_x", 4, "i32", "td5_actor_struct.h:418"),
    0x2C0: ("wheel_world_hires_RR_y", 4, "i32", "td5_actor_struct.h:418"),
    0x2C4: ("wheel_world_hires_RR_z", 4, "i32", "td5_actor_struct.h:418"),
    
    # Clean Driving Score (0x2C8-0x2CB)
    0x2C8: ("clean_driving_score", 4, "i32", "td5_actor_struct.h:421"),
    
    # Center Suspension (0x2CC-0x2DB)
    0x2CC: ("center_suspension_pos", 4, "i32", "td5_actor_struct.h:427"),
    0x2D0: ("center_suspension_vel", 4, "i32", "td5_actor_struct.h:428"),
    0x2D8: ("prev_frame_y_position", 4, "i32", "td5_actor_struct.h:430"),
    
    # Per-Wheel Suspension (0x2DC-0x30B)
    0x2DC: ("wheel_suspension_pos_FL", 4, "i32", "td5_actor_struct.h:438"),
    0x2E0: ("wheel_suspension_pos_FR", 4, "i32", "td5_actor_struct.h:438"),
    0x2E4: ("wheel_suspension_pos_RL", 4, "i32", "td5_actor_struct.h:438"),
    0x2E8: ("wheel_suspension_pos_RR", 4, "i32", "td5_actor_struct.h:438"),
    0x2EC: ("wheel_spring_dv_FL", 4, "i32", "td5_actor_struct.h:439"),
    0x2F0: ("wheel_spring_dv_FR", 4, "i32", "td5_actor_struct.h:439"),
    0x2F4: ("wheel_spring_dv_RL", 4, "i32", "td5_actor_struct.h:439"),
    0x2F8: ("wheel_spring_dv_RR", 4, "i32", "td5_actor_struct.h:439"),
    0x2FC: ("wheel_load_accum_FL", 4, "i32", "td5_actor_struct.h:441"),
    0x300: ("wheel_load_accum_FR", 4, "i32", "td5_actor_struct.h:441"),
    0x304: ("wheel_load_accum_RL", 4, "i32", "td5_actor_struct.h:441"),
    0x308: ("wheel_load_accum_RR", 4, "i32", "td5_actor_struct.h:441"),
    
    # Steering & Engine (0x30C-0x323)
    0x30C: ("steering_command", 4, "i32", "td5_ai.c:105 / td5_actor_struct.h:448"),
    0x310: ("engine_speed_accum", 4, "i32", "td5_actor_struct.h:450"),
    0x314: ("longitudinal_speed", 4, "i32", "td5_ai.c:106 / td5_actor_struct.h:452"),
    0x318: ("lateral_speed", 4, "i32", "td5_actor_struct.h:454"),
    0x31C: ("front_axle_slip_excess", 4, "i32", "td5_actor_struct.h:456"),
    0x320: ("rear_axle_slip_excess", 4, "i32", "td5_ai.c:107 / td5_actor_struct.h:458"),
    
    # Scoring & Metrics (0x324-0x337)
    0x324: ("cached_car_suspension_travel", 4, "i32", "td5_actor_struct.h:464"),
    0x328: ("finish_time", 4, "i32", "td5_actor_struct.h:466"),
    0x32C: ("accumulated_distance", 4, "i32", "td5_actor_struct.h:469"),
    0x330: ("peak_speed", 2, "i16", "td5_actor_struct.h:471"),
    0x332: ("average_speed_metric", 2, "i16", "td5_actor_struct.h:472"),
    0x334: ("finish_time_aux", 2, "i16", "td5_actor_struct.h:473"),
    0x336: ("finish_time_subtick", 2, "i16", "td5_actor_struct.h:474"),
    
    # Frame Counter & Timers (0x338-0x345)
    0x338: ("frame_counter", 2, "i16", "td5_actor_struct.h:480"),
    0x33A: ("steering_ramp_accumulator", 2, "i16", "td5_ai.c:108 / td5_actor_struct.h:483"),
    0x33C: ("current_slip_metric", 2, "i16", "td5_actor_struct.h:486"),
    0x33E: ("encounter_steering_cmd", 2, "i16", "td5_ai.c:113 / td5_actor_struct.h:488"),
    0x340: ("accumulated_tire_slip_x", 2, "i16", "td5_actor_struct.h:491"),
    0x342: ("accumulated_tire_slip_z", 2, "i16", "td5_actor_struct.h:492"),
    0x344: ("pending_finish_timer", 2, "u16", "td5_actor_struct.h:493"),
    
    # Checkpoint & Race State (0x34E-0x35F)
    0x34E: ("checkpoint_split_times", 18, "bytes", "td5_actor_struct.h:502"),
    0x360: ("airborne_frame_counter", 2, "u16", "td5_trace.h:158 / td5_actor_struct.h:506"),
    
    # Gear & Control Flags (0x36B-0x374)
    0x36B: ("current_gear", 1, "u8", "td5_actor_struct.h:514"),
    0x36C: ("max_gear_index", 1, "u8", "td5_actor_struct.h:516"),
    0x36D: ("brake_flag", 1, "u8", "td5_ai.c:114 / td5_actor_struct.h:519"),
    0x36E: ("handbrake_flag", 1, "u8", "td5_actor_struct.h:521"),
    0x36F: ("throttle_state", 1, "u8", "td5_ai.c:115 / td5_actor_struct.h:523"),
    0x370: ("surface_type_chassis", 1, "u8", "td5_actor_struct.h:525"),
    0x371: ("tire_track_emitter_FL", 1, "u8", "td5_actor_struct.h:527"),
    0x372: ("tire_track_emitter_FR", 1, "u8", "td5_actor_struct.h:528"),
    0x373: ("tire_track_emitter_RL", 1, "u8", "td5_actor_struct.h:529"),
    0x374: ("tire_track_emitter_RR", 1, "u8", "td5_actor_struct.h:530"),
    
    # Slot Index & State Flags (0x375-0x383)
    0x375: ("slot_index", 1, "u8", "td5_ai.c:116 / td5_actor_struct.h:538"),
    0x376: ("surface_contact_flags", 1, "u8", "td5_trace.h:159 / td5_actor_struct.h:540"),
    0x378: ("throttle_input_active", 1, "u8", "td5_actor_struct.h:543"),
    0x379: ("vehicle_mode", 1, "u8", "td5_ai.c:117 / td5_trace.h:160 / td5_actor_struct.h:546"),
    0x37B: ("track_contact_flag", 1, "u8", "td5_actor_struct.h:550"),
    0x37C: ("wheel_contact_bitmask", 1, "u8", "td5_trace.h:158 / td5_actor_struct.h:553"),
    0x37D: ("damage_lockout", 1, "u8", "td5_actor_struct.h:558"),
    0x37E: ("ghost_flag", 1, "u8", "td5_actor_struct.h:566"),
    0x380: ("grip_reduction", 1, "u8", "td5_actor_struct.h:572"),
    0x381: ("prev_race_position", 1, "u8", "td5_actor_struct.h:574"),
    0x383: ("race_position", 1, "u8", "td5_actor_struct.h:578"),
    
    # Special Encounter State (0x384-0x387)
    0x384: ("special_encounter_state", 4, "i32", "td5_ai.c:118 / td5_actor_struct.h:581"),
}

# Struct size and highest documented offsets
ACTOR_STRIDE = 0x388

# Top 5 highest documented offsets (coverage tail):
# 0x384 special_encounter_state (4 bytes, end at 0x388)
# 0x383 race_position (1 byte)
# 0x381 prev_race_position (1 byte)
# 0x380 grip_reduction (1 byte)
# 0x37E ghost_flag (1 byte)
