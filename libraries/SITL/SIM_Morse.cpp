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
/*
  simulator connector for morse simulator
*/

#include "SIM_Morse.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <AP_HAL/AP_HAL.h>
#include <DataFlash/DataFlash.h>
#include "pthread.h"

extern const AP_HAL::HAL& hal;

using namespace SITL;

static const struct {
    const char *name;
    float value;
    bool save;
} sim_defaults[] = {
    { "AHRS_EKF_TYPE", 10 },
    { "INS_GYR_CAL", 0 },
    { "RC1_MIN", 1000, true },
    { "RC1_MAX", 2000, true },
    { "RC2_MIN", 1000, true },
    { "RC2_MAX", 2000, true },
    { "RC3_MIN", 1000, true },
    { "RC3_MAX", 2000, true },
    { "RC4_MIN", 1000, true },
    { "RC4_MAX", 2000, true },
    { "RC2_REVERSED", 1 }, // interlink has reversed rc2
    { "SERVO1_MIN", 1000 },
    { "SERVO1_MAX", 2000 },
    { "SERVO2_MIN", 1000 },
    { "SERVO2_MAX", 2000 },
    { "SERVO3_MIN", 1000 },
    { "SERVO3_MAX", 2000 },
    { "SERVO4_MIN", 1000 },
    { "SERVO4_MAX", 2000 },
    { "SERVO5_MIN", 1000 },
    { "SERVO5_MAX", 2000 },
    { "SERVO6_MIN", 1000 },
    { "SERVO6_MAX", 2000 },
    { "SERVO6_MIN", 1000 },
    { "SERVO6_MAX", 2000 },
    { "INS_ACC2OFFS_X",    0.001 },
    { "INS_ACC2OFFS_Y",    0.001 },
    { "INS_ACC2OFFS_Z",    0.001 },
    { "INS_ACC2SCAL_X",    1.001 },
    { "INS_ACC2SCAL_Y",    1.001 },
    { "INS_ACC2SCAL_Z",    1.001 },
    { "INS_ACCOFFS_X",     0.001 },
    { "INS_ACCOFFS_Y",     0.001 },
    { "INS_ACCOFFS_Z",     0.001 },
    { "INS_ACCSCAL_X",     1.001 },
    { "INS_ACCSCAL_Y",     1.001 },
    { "INS_ACCSCAL_Z",     1.001 },
};


Morse::Morse(const char *home_str, const char *frame_str) :
    Aircraft(home_str, frame_str)
{
    const char *colon = strchr(frame_str, ':');
    if (colon) {
        morse_ip = colon+1;
    }

    if (strstr(frame_str, "-rover")) {
        output_type = OUTPUT_ROVER;
    } else if (strstr(frame_str, "-quad")) {
        output_type = OUTPUT_QUAD;
    } else {
        // default to rover
        output_type = OUTPUT_ROVER;
    }

    for (uint8_t i=0; i<ARRAY_SIZE(sim_defaults); i++) {
        AP_Param::set_default_by_name(sim_defaults[i].name, sim_defaults[i].value);
        if (sim_defaults[i].save) {
            enum ap_var_type ptype;
            AP_Param *p = AP_Param::find(sim_defaults[i].name, &ptype);
            if (!p->configured()) {
                p->save();
            }
        }
    }
    printf("Started Morse backend\n");
}

/*
  very simple JSON parser for sensor data
  called with pointer to one row of sensor data, nul terminated

  This parser does not do any syntax checking, and is not at all
  general purpose
*/
bool Morse::parse_sensors(const char *json)
{
    //printf("%s\n", json);
    for (uint16_t i=0; i<ARRAY_SIZE(keytable); i++) {
        struct keytable &key = keytable[i];

        /* look for section header */
        const char *p = strstr(json, key.section);
        if (!p) {
            printf("Failed to find section %s\n", key.section);
            return false;
        }
        p += strlen(key.section)+1;

        // find key inside section
        p = strstr(p, key.key);
        if (!p) {
            printf("Failed to find key %s/%s\n", key.section, key.key);
            return false;
        }

        p += strlen(key.key)+2;
        if (key.is_vector3) {
            p += 2;
            if (sscanf(p, "%lf, %lf, %lf", &key.ptr[0], &key.ptr[1], &key.ptr[2]) != 3) {
                printf("Failed to parse vector3 for %s/%s\n", key.section, key.key);
                return false;
            }
            //printf("%s.%s [%f, %f, %f]\n", key.section, key.key, key.ptr[0], key.ptr[1], key.ptr[2]);
        } else {
            key.ptr[0] = atof(p);
            //printf("%s.%s %f\n", key.section, key.key, *key.ptr);
        }
    }
    socket_frame_counter++;
    return true;
}

/*
  connect to the required sockets
 */
bool Morse::connect_sockets(void)
{
    if (!sensors_sock) {
        sensors_sock = new SocketAPM(false);
        if (!sensors_sock) {
            AP_HAL::panic("Out of memory for sensors socket");
        }
        if (!sensors_sock->connect(morse_ip, morse_sensors_port)) {
            if (connect_counter++ == 1000) {
                printf("Waiting to connect to sensors control on %s:%u\n",
                       morse_ip, morse_sensors_port);
                connect_counter = 0;
            }
            delete sensors_sock;
            sensors_sock = nullptr;
            return false;
        }
        sensors_sock->reuseaddress();
        printf("Sensors connected\n");
    }
    if (!control_sock) {
        control_sock = new SocketAPM(false);
        if (!control_sock) {
            AP_HAL::panic("Out of memory for control socket");
        }
        if (!control_sock->connect(morse_ip, morse_control_port)) {
            if (connect_counter++ == 1000) {
                printf("Waiting to connect to control control on %s:%u\n",
                       morse_ip, morse_control_port);
                connect_counter = 0;
            }
            delete control_sock;
            control_sock = nullptr;
            return false;
        }
        control_sock->reuseaddress();
        printf("Control connected\n");
    }
    return true;
}

/*
  get any new data from the sensors socket
*/
bool Morse::sensors_receive(void)
{
    ssize_t ret = sensors_sock->recv(&sensor_buffer[sensor_buffer_len], sizeof(sensor_buffer)-sensor_buffer_len, 0);
    if (ret <= 0) {
        no_data_counter++;
        if (no_data_counter == 1000) {
            no_data_counter = 0;
            delete sensors_sock;
            delete control_sock;
            sensors_sock = nullptr;
            control_sock = nullptr;
        }
        return false;
    }
    no_data_counter = 0;

    // convert '\n' into nul
    while (uint8_t *p = (uint8_t *)memchr(&sensor_buffer[sensor_buffer_len], '\n', ret)) {
        *p = 0;
    }
    sensor_buffer_len += ret;

    const uint8_t *p2 = (const uint8_t *)memrchr(sensor_buffer, 0, sensor_buffer_len);
    if (p2 == nullptr || p2 == sensor_buffer) {
        return false;
    }
    const uint8_t *p1 = (const uint8_t *)memrchr(sensor_buffer, 0, p2 - sensor_buffer);
    if (p1 == nullptr) {
        return false;
    }

    bool parse_ok = parse_sensors((const char *)(p1+1));

    memmove(sensor_buffer, p2, sensor_buffer_len - (p2 - sensor_buffer));
    sensor_buffer_len = sensor_buffer_len - (p2 - sensor_buffer);

    return parse_ok;
}

/*
  output control command assuming skid-steering rover
 */
void Morse::output_rover(const struct sitl_input &input)
{
    float motor1 = 2*((input.servos[0]-1000)/1000.0f - 0.5f);
    float motor2 = 2*((input.servos[2]-1000)/1000.0f - 0.5f);
    const float steer_rate_max_dps = 60;
    const float max_speed = 2;

    const float steering_rps = (motor1 - motor2) * radians(steer_rate_max_dps);
    const float speed_ms = 0.5*(motor1 + motor2) * max_speed;

    // construct a JSON packet for v and w
    char buf[60];
    snprintf(buf, sizeof(buf)-1, "{\"v\": %.3f, \"w\": %.2f}\n",
             speed_ms, -steering_rps);
    buf[sizeof(buf)-1] = 0;

    control_sock->send(buf, strlen(buf));
}

/*
  output control command assuming a 4 channel quad
 */
void Morse::output_quad(const struct sitl_input &input)
{
    const float max_thrust = 1500;
    float motors[4];
    for (uint8_t i=0; i<4; i++) {
        motors[i] = constrain_float(((input.servos[i]-1000)/1000.0f) * max_thrust, 0, max_thrust);
    }
    const float &m_right = motors[0];
    const float &m_left  = motors[1];
    const float &m_front = motors[2];
    const float &m_back  = motors[3];

    // quad format in Morse is:
    // m1: back
    // m2: right
    // m3: front
    // m4: left

    // construct a JSON packet for motors
    char buf[60];
    snprintf(buf, sizeof(buf)-1, "{\"engines\": [%.3f, %.3f, %.3f, %.3f]}\n",
             m_back, m_right, m_front, m_left);
    buf[sizeof(buf)-1] = 0;

    control_sock->send(buf, strlen(buf));
}


/*
  update the Morse simulation by one time step
 */
void Morse::update(const struct sitl_input &input)
{
    if (!connect_sockets()) {
        return;
    }

    last_state = state;

    if (sensors_receive()) {
        // update average frame time used for extrapolation
        double dt = constrain_float(state.timestamp - last_state.timestamp, 0.001, 1.0/50);
        if (average_frame_time_s < 1.0e-6) {
            average_frame_time_s = dt;
        }
        average_frame_time_s = average_frame_time_s * 0.98 + dt * 0.02;
    }

    double dt_s = state.timestamp - last_state.timestamp;
    if (dt_s < 0 || dt_s > 1) {
        // cope with restarting while connected
        initial_time_s = time_now_us * 1.0e-6f;
        last_time_s = state.timestamp;
        position_offset.zero();
        return;
    }

    if (dt_s < 0.00001f) {
        float delta_time = 0.001;
        // don't go past the next expected frame
        if (delta_time + extrapolated_s > average_frame_time_s) {
            delta_time = average_frame_time_s - extrapolated_s;
        }
        if (delta_time <= 0) {
            usleep(1000);
            return;
        }
        time_now_us += delta_time * 1.0e6;
        extrapolate_sensors(delta_time);
        update_position();
        update_mag_field_bf();
        usleep(delta_time*1.0e6);
        extrapolated_s += delta_time;
        report_FPS();
        return;
    }

    extrapolated_s = 0;
    
    if (initial_time_s <= 0) {
        dt_s = 0.001f;
        initial_time_s = state.timestamp - dt_s;
    }

    // convert from state variables to ardupilot conventions
    dcm.from_euler(state.pose.roll, -state.pose.pitch, -state.pose.yaw);

    gyro = Vector3f(state.imu.angular_velocity[0],
                    -state.imu.angular_velocity[1],
                    -state.imu.angular_velocity[2]);

    velocity_ef = Vector3f(state.velocity.world_linear_velocity[0],
                           -state.velocity.world_linear_velocity[1],
                           -state.velocity.world_linear_velocity[2]);

    position = Vector3f(state.gps.x, -state.gps.y, -state.gps.z);

    // Morse IMU accel is NEU, convert to NED
    accel_body = Vector3f(state.imu.linear_acceleration[0],
                          -state.imu.linear_acceleration[1],
                          -state.imu.linear_acceleration[2]);

    // limit to 16G to match pixhawk1
    float a_limit = GRAVITY_MSS*16;
    accel_body.x = constrain_float(accel_body.x, -a_limit, a_limit);
    accel_body.y = constrain_float(accel_body.y, -a_limit, a_limit);
    accel_body.z = constrain_float(accel_body.z, -a_limit, a_limit);

    // offset based on first position to account for offset in morse world
    if (position_offset.is_zero()) {
        position_offset = position;
    }
    position -= position_offset;

    update_position();
    time_advance();
    uint64_t new_time_us = (state.timestamp - initial_time_s)*1.0e6;
    if (new_time_us < time_now_us) {
        uint64_t dt_us = time_now_us - new_time_us;
        if (dt_us > 500000) {
            // time going backwards
            time_now_us = new_time_us;
        }
    } else {
        time_now_us = new_time_us;
    }

    last_time_s = state.timestamp;

    // update magnetic field
    update_mag_field_bf();

    switch (output_type) {
    case OUTPUT_ROVER:
        output_rover(input);
        break;
    case OUTPUT_QUAD:
        output_quad(input);
        break;
    }

    report_FPS();
}


/*
  report frame rates
 */
void Morse::report_FPS(void)
{
    if (frame_counter++ % 1000 == 0) {
        if (!is_zero(last_frame_count_s)) {
            uint64_t frames = socket_frame_counter - last_socket_frame_counter;
            last_socket_frame_counter = socket_frame_counter;
            double dt = state.timestamp - last_frame_count_s;
            printf("%.2f/%.2f FPS avg=%.2f\n",
                   frames / dt, 1000 / dt, 1.0/average_frame_time_s);
        } else {
            printf("Initial position %f %f %f\n", position.x, position.y, position.z);
        }
        last_frame_count_s = state.timestamp;
    }
}
