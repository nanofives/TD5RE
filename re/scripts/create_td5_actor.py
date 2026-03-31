# Ghidra script to create TD5_Actor struct (904 bytes / 0x388)
# @category TD5RE
# @author TD5RE project

from ghidra.program.model.data import StructureDataType, CategoryPath, ArrayDataType
from ghidra.program.model.data import IntegerDataType, ShortDataType, ByteDataType, FloatDataType, PointerDataType
from ghidra.program.model.data import DataTypeConflictHandler

dtm = currentProgram.getDataTypeManager()
cat = CategoryPath("/TD5")

# Lookup sub-types
def find_type(name):
    it = dtm.getAllDataTypes()
    while it.hasNext():
        dt = it.next()
        if dt.getName() == name:
            return dt
    return None

vec3fixed = find_type("TD5_Vec3_Fixed")
vec3float = find_type("TD5_Vec3_Float")
mat3x3 = find_type("TD5_Mat3x3")
euler = find_type("TD5_EulerAccum")
dispang = find_type("TD5_DisplayAngles")

INT = IntegerDataType()
SHORT = ShortDataType()
BYTE = ByteDataType()
FLOAT = FloatDataType()
PTR = PointerDataType()

# Find uchar type
ubyte = find_type("uchar")
if ubyte is None:
    ubyte = BYTE
ushort_t = find_type("ushort")
if ushort_t is None:
    ushort_t = SHORT

println("Sub-types found: vec3fixed=%s mat3x3=%s euler=%s dispang=%s" % (vec3fixed, mat3x3, euler, dispang))

actor = StructureDataType(cat, "TD5_Actor", 0x388)

# Track position state
actor.replaceAtOffset(0x080, SHORT, 2, "track_span_raw", "current STRIP.DAT span index")
actor.replaceAtOffset(0x082, SHORT, 2, "track_span_normalized", "span modulo ring length")
actor.replaceAtOffset(0x084, SHORT, 2, "track_span_accumulated", "monotonic forward span")
actor.replaceAtOffset(0x086, SHORT, 2, "track_span_high_water", "race ordering high-water")
actor.replaceAtOffset(0x08C, ubyte, 1, "track_sub_lane_index", "sub-lane within span")

# Contact probes
if vec3fixed:
    actor.replaceAtOffset(0x090, vec3fixed, vec3fixed.getLength(), "probe_FL", "front-left wheel probe")
    actor.replaceAtOffset(0x09C, vec3fixed, vec3fixed.getLength(), "probe_FR", "front-right wheel probe")
    actor.replaceAtOffset(0x0A8, vec3fixed, vec3fixed.getLength(), "probe_RL", "rear-left wheel probe")
    actor.replaceAtOffset(0x0B4, vec3fixed, vec3fixed.getLength(), "probe_RR", "rear-right wheel probe")

# Collision spin matrix
if mat3x3:
    actor.replaceAtOffset(0x0C0, mat3x3, mat3x3.getLength(), "collision_spin_matrix", "recovery rotation")

# Wheel track contacts
arr_short_24 = ArrayDataType(SHORT, 24, 2)
actor.replaceAtOffset(0x0F0, arr_short_24, 48, "wheel_track_contacts", "per-wheel track contact state")

# Main rotation matrix
if mat3x3:
    actor.replaceAtOffset(0x120, mat3x3, mat3x3.getLength(), "rotation_matrix", "primary orientation")

# Render position
if vec3float:
    actor.replaceAtOffset(0x144, vec3float, vec3float.getLength(), "render_pos", "float render position")

# Saved orientation
if mat3x3:
    actor.replaceAtOffset(0x150, mat3x3, mat3x3.getLength(), "saved_orientation", "saved orientation for recovery")
    actor.replaceAtOffset(0x180, mat3x3, mat3x3.getLength(), "recovery_target_matrix", "recovery interpolation target")

# Car config pointers
actor.replaceAtOffset(0x1B0, PTR, 4, "car_config_ptr", "car visual config")
actor.replaceAtOffset(0x1B8, PTR, 4, "car_definition_ptr", "car definition struct")
actor.replaceAtOffset(0x1BC, PTR, 4, "tuning_data_ptr", "tuning/physics params")

# Velocities
actor.replaceAtOffset(0x1C0, INT, 4, "angular_velocity_roll", "roll angular velocity")
actor.replaceAtOffset(0x1C4, INT, 4, "angular_velocity_yaw", "yaw angular velocity")
actor.replaceAtOffset(0x1C8, INT, 4, "angular_velocity_pitch", "pitch angular velocity")
actor.replaceAtOffset(0x1CC, INT, 4, "linear_velocity_x", "world-space linear velocity X")
actor.replaceAtOffset(0x1D0, INT, 4, "linear_velocity_y", "world-space linear velocity Y")
actor.replaceAtOffset(0x1D4, INT, 4, "linear_velocity_z", "world-space linear velocity Z")

# Euler accumulators
if euler:
    actor.replaceAtOffset(0x1F0, euler, euler.getLength(), "euler_accum", "euler angle accumulators")

# World position
if vec3fixed:
    actor.replaceAtOffset(0x1FC, vec3fixed, vec3fixed.getLength(), "world_pos", "world position 24.8 fixed")

# Display angles
if dispang:
    actor.replaceAtOffset(0x208, dispang, dispang.getLength(), "display_angles", "12-bit display angles")

# Center suspension
actor.replaceAtOffset(0x2CC, INT, 4, "center_suspension_pos", "chassis suspension position")
actor.replaceAtOffset(0x2D0, INT, 4, "center_suspension_vel", "chassis suspension velocity")
actor.replaceAtOffset(0x2D8, INT, 4, "prev_frame_y_position", "previous frame world_pos.y")

# Per-wheel suspension arrays
arr4int = ArrayDataType(INT, 4, 4)
actor.replaceAtOffset(0x2DC, arr4int, 16, "wheel_suspension_pos", "per-wheel spring deflection")
actor.replaceAtOffset(0x2EC, arr4int, 16, "wheel_force_accum", "per-wheel force accumulators")
actor.replaceAtOffset(0x2FC, arr4int, 16, "wheel_suspension_vel", "per-wheel spring-damper velocity")

# Steering & engine
actor.replaceAtOffset(0x30C, INT, 4, "steering_command", "steering angle")
actor.replaceAtOffset(0x310, INT, 4, "engine_speed_accum", "engine RPM accumulator")
actor.replaceAtOffset(0x314, INT, 4, "longitudinal_speed", "body-frame forward speed")
actor.replaceAtOffset(0x318, INT, 4, "lateral_speed", "body-frame lateral speed")
actor.replaceAtOffset(0x31C, INT, 4, "front_axle_slip_excess", "front axle slip")
actor.replaceAtOffset(0x320, INT, 4, "rear_axle_slip_excess", "rear axle slip")

# Scoring
actor.replaceAtOffset(0x328, INT, 4, "finish_time", "cumulative timer at finish")
actor.replaceAtOffset(0x32C, INT, 4, "accumulated_distance", "odometer metric")
actor.replaceAtOffset(0x330, SHORT, 2, "peak_speed", "maximum speed achieved")
actor.replaceAtOffset(0x332, SHORT, 2, "average_speed_metric", "running average speed")
actor.replaceAtOffset(0x334, SHORT, 2, "finish_time_aux", "post-finish timing")
actor.replaceAtOffset(0x336, SHORT, 2, "finish_time_subtick", "sub-tick precision")

# Frame counter & timers
actor.replaceAtOffset(0x338, SHORT, 2, "frame_counter", "frames since spawn/reset")
actor.replaceAtOffset(0x33E, SHORT, 2, "encounter_steering_cmd", "traffic/encounter speed")
actor.replaceAtOffset(0x340, SHORT, 2, "accumulated_tire_slip_x", "tire slip X")
actor.replaceAtOffset(0x342, SHORT, 2, "accumulated_tire_slip_z", "tire slip Z")
actor.replaceAtOffset(0x344, ushort_t, 2, "pending_finish_timer", "checkpoint countdown")
actor.replaceAtOffset(0x34C, SHORT, 2, "timing_frame_counter", "timing frame counter")

# Gear & flags
actor.replaceAtOffset(0x36B, ubyte, 1, "current_gear", "0=R,1=N,2+=fwd")
actor.replaceAtOffset(0x36D, ubyte, 1, "brake_flag", "nonzero = braking")
actor.replaceAtOffset(0x36E, ubyte, 1, "handbrake_flag", "nonzero = handbrake")
actor.replaceAtOffset(0x370, ubyte, 1, "surface_type_chassis", "ground surface type")
actor.replaceAtOffset(0x375, ubyte, 1, "slot_index", "actor slot 0-11")
actor.replaceAtOffset(0x376, ubyte, 1, "surface_contact_flags", "surface contact bitmask")
actor.replaceAtOffset(0x379, ubyte, 1, "vehicle_mode", "0=normal, 1=scripted")
actor.replaceAtOffset(0x37B, ubyte, 1, "track_contact_flag", "V2W contact flag")
actor.replaceAtOffset(0x37C, ubyte, 1, "damage_lockout", "damage lockout counter")
actor.replaceAtOffset(0x37D, ubyte, 1, "wheel_contact_bitmask", "per-wheel ground contact bits")
actor.replaceAtOffset(0x37E, ubyte, 1, "ghost_flag", "time trial ghost flag")
actor.replaceAtOffset(0x380, ubyte, 1, "grip_reduction", "grip reduction override")
actor.replaceAtOffset(0x383, ubyte, 1, "race_position", "0=1st")
actor.replaceAtOffset(0x384, INT, 4, "special_encounter_state", "encounter completion counter")

# Commit to data type manager
tid = dtm.startTransaction("Add TD5_Actor")
try:
    result = dtm.addDataType(actor, DataTypeConflictHandler.REPLACE_HANDLER)
    dtm.endTransaction(tid, True)
    println("SUCCESS: TD5_Actor created with %d bytes" % result.getLength())
except Exception as e:
    dtm.endTransaction(tid, False)
    println("FAILED: %s" % str(e))
