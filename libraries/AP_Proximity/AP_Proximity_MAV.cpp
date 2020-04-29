/*
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <AP_HAL/AP_HAL.h>
#include "AP_Proximity_MAV.h"
#include <ctype.h>
#include <stdio.h>

extern const AP_HAL::HAL& hal;

#define PROXIMITY_MAV_TIMEOUT_MS    500 // distance messages must arrive within this many milliseconds

uint8_t AP_Proximity_MAV::get_object_count() const
{
    return 1;
}

bool AP_Proximity_MAV::get_object_angle_and_distance(uint8_t object_number, float& angle_deg, float &distance) const
{
    if (object_number < 1) {
        angle_deg = _angle_deg;
        distance = _dist;
        return true;
    } else {
        return false;
    } 
}

// update the state of the sensor
void AP_Proximity_MAV::update(void)
{
    // check for timeout and set health status
    if ((_last_update_ms == 0 || (AP_HAL::millis() - _last_update_ms > PROXIMITY_MAV_TIMEOUT_MS)) &&
        (_last_upward_update_ms == 0 || (AP_HAL::millis() - _last_upward_update_ms > PROXIMITY_MAV_TIMEOUT_MS))) {
        set_status(AP_Proximity::Status::NoData);
    } else {
        set_status(AP_Proximity::Status::Good);
    }
}

// get distance upwards in meters. returns true on success
bool AP_Proximity_MAV::get_upward_distance(float &distance) const
{
    if ((_last_upward_update_ms != 0) && (AP_HAL::millis() - _last_upward_update_ms <= PROXIMITY_MAV_TIMEOUT_MS)) {
        distance = _distance_upward;
        return true;
    }
    return false;
}

// handle mavlink DISTANCE_SENSOR messages
void AP_Proximity_MAV::handle_msg(const mavlink_message_t &msg)
{
    if (msg.msgid == MAVLINK_MSG_ID_DISTANCE_SENSOR) {
        mavlink_distance_sensor_t packet;
        mavlink_msg_distance_sensor_decode(&msg, &packet);

        // store distance to appropriate sector based on orientation field
        _angle_deg = packet.orientation * 2;
        _dist = packet.current_distance * 0.01f;
        _distance_min = packet.min_distance * 0.01f;
        _distance_max = packet.max_distance * 0.01f;
        _last_update_ms = AP_HAL::millis();

        return;
    }

    if (msg.msgid == MAVLINK_MSG_ID_OBSTACLE_DISTANCE) {
        mavlink_obstacle_distance_t packet;
        mavlink_msg_obstacle_distance_decode(&msg, &packet);

        // check increment (message's sector width)
        float increment;
        if (!is_zero(packet.increment_f)) {
            // use increment float
            increment = packet.increment_f;
        } else if (packet.increment != 0) {
            // use increment uint8_t
            increment = packet.increment;
        } else {
            // invalid increment
            return;
        }

        const float MAX_DISTANCE = 9999.0f;
        const uint8_t total_distances = MIN(((360.0f / fabsf(increment)) + 0.5f), MAVLINK_MSG_OBSTACLE_DISTANCE_FIELD_DISTANCES_LEN); // usually 72

        // set distance min and max
        _distance_min = packet.min_distance * 0.01f;
        _distance_max = packet.max_distance * 0.01f;
        _last_update_ms = AP_HAL::millis();

        // get user configured yaw correction from front end
        const float param_yaw_offset = constrain_float(frontend.get_yaw_correction(state.instance), -360.0f, +360.0f);
        const float yaw_correction = wrap_360(param_yaw_offset + packet.angle_offset);
        if (frontend.get_orientation(state.instance) != 0) {
            increment *= -1;
        }

        Vector2f current_pos;
        float current_heading;
        const bool database_ready = database_prepare_for_push(current_pos, current_heading);

        // initialise updated array and proximity sector angles (to closest object) and distances
        bool sector_updated[PROXIMITY_NUM_SECTORS];
        for (uint8_t i = 0; i < PROXIMITY_NUM_SECTORS; i++) {
            sector_updated[i] = false;
            _angle[i] = _sector_middle_deg[i];
            _distance[i] = MAX_DISTANCE;
        }

        // iterate over message's sectors
        for (uint8_t j = 0; j < total_distances; j++) {
            const uint16_t distance_cm = packet.distances[j];
            if (distance_cm == 0 ||
                distance_cm == 65535 ||
                distance_cm < packet.min_distance ||
                distance_cm > packet.max_distance)
            {
                // sanity check failed, ignore this distance value
                continue;
            }

            const float packet_distance_m = distance_cm * 0.01f;
            const float mid_angle = wrap_360((float)j * increment + yaw_correction);

            // iterate over proximity sectors
            for (uint8_t i = 0; i < PROXIMITY_NUM_SECTORS; i++) {
                float angle_diff = fabsf(wrap_180(_sector_middle_deg[i] - mid_angle));
                // update distance array sector with shortest distance from message
                if ((angle_diff <= (PROXIMITY_SECTOR_WIDTH_DEG * 0.5f)) && (packet_distance_m < _distance[i])) {
                    _distance[i] = packet_distance_m;
                    _angle[i] = mid_angle;
                    sector_updated[i] = true;
                }
            }

            // update Object Avoidance database with Earth-frame point
            if (database_ready) {
                database_push(mid_angle, packet_distance_m, _last_update_ms, current_pos, current_heading);
            }
        }

        // update proximity sectors validity and boundary point
        for (uint8_t i = 0; i < PROXIMITY_NUM_SECTORS; i++) {
            _distance_valid[i] = (_distance[i] >= _distance_min) && (_distance[i] <= _distance_max);
            if (sector_updated[i]) {
                update_boundary_for_sector(i, false);
            }
        }
    }
}
