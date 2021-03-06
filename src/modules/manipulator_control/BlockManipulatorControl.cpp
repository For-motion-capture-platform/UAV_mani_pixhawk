/*
 * BlockManipulatorControl.cpp
 *
 *  Created on: 7 Nov 2016
 *      Author: bdai <bdai1412@gmailcom>
 */

#include <drivers/drv_hrt.h>
#include <systemlib/mavlink_log.h>
#include <fcntl.h>
#include <matrix/math.hpp>

#include "BlockManipulatorControl.hpp"

static float GRAPPER_ANGLE_RANGE[2] = {-10.0f / 180.0f * (float)M_PI, 10.0f / 180.0f * (float)M_PI};

static float MANI_RANGE[6] = {0.36f, 0.46f,
		-60.0f / 180.0f * (float)M_PI, -5.0f / 180.0f * (float)M_PI, //must set behind 0
		-35.0f / 180.0f * (float)M_PI, 35.0f / 180.0f * (float)M_PI};
enum {R_MIN = 0, R_MAX, THETA_MIN, THETA_MAX, PSI_MIN, PSI_MAX};

/*relative rest duration before start grabbing 0.5s -bdai<13 Nov 2016>*/
static uint32_t REST_DURATION = 1000000;	// 0.5 s
static float ACCURACY = 0.03f;	//grab demand 0.05m accuracy
static orb_advert_t mavlink_log_pub = nullptr;
/*used for manipulator velocity cointrol -bdai<19 Nov 2016>*/
static float MANI_P = 1.0f;
static float MANI_MAX_VEL = 0.05f;

BlockManipulatorControl::BlockManipulatorControl():
	//this block has no parent, and has name MANC
	SuperBlock(NULL, "MANC"),
	// subscriptions, set rate, add to list
	_pos_sub(ORB_ID(vehicle_local_position), 1000 / 20, 0, &getSubscriptions()),
	_att_sub(ORB_ID(vehicle_attitude), 1000 / 20, 0, &getSubscriptions()),
	_manual_sub(ORB_ID(manual_control_setpoint), 1000 / 20, 0, &getSubscriptions()),
	_target_sub(ORB_ID(target_info), 1000/20, 0, &getSubscriptions()),
	_mani_status_sub(ORB_ID(endeff_frame_status), 1000 / 20 , 0, &getSubscriptions()),
	// publications
	_manip_pub(ORB_ID(target_endeff_frame), -1, &getPublications()),
	_polls(),
	// Loop performance
	_err_perf(),
	_timeStamp(),
	_timePrint(),
	_mani_triggered(false),
	_in_range(0),
	_relative_rest(false),
	_relative_rest_time(),
	_last_info_time(0),
	_grabbed(false)
{
	_polls[0].fd = _target_sub.getHandle();
	_polls[0].events = POLLIN;

	// initialize paramet3ers dependent matrices
//	updateParams();
}

BlockManipulatorControl::~BlockManipulatorControl()
{

}

void BlockManipulatorControl::control()
{
	//wait for local position update
	int ret = px4_poll(_polls, 1, 200);

	if (ret < 0)
	{
		perf_count(_err_perf);
		warnx("error");
		return;
	}

	if (ret == 0) {
		warnx("tiem out");
		return;
	}

//	usleep(50000);
	_timeStamp = hrt_absolute_time();
	bool print = (_timeStamp - _timePrint) / 1.0e6f > 0.5f;
	if (print) _timePrint = _timeStamp;

	// get new data
	updateSubscriptions();

	bool enableMani = (_manual_sub.get().flaps > 0.75f);
	enableMani = true;

	if (enableMani) {
		Vector3f pos(_pos_sub.get().x, _pos_sub.get().y, _pos_sub.get().z);
//		pos = Vector3f(.0f, .0f, .0f);
		print_info(print, &mavlink_log_pub, "pos x:%8.4f, y:%8.4f, z:%8.4f",
					(double)pos(0),
					(double)pos(1),
					(double)pos(2));
		Vector3f target_pos(_target_sub.get().x, _target_sub.get().y, _target_sub.get().z);
//		target_pos = Vector3f(0.25f, 0.0f, 0.5f);

//		Quatf qq;
//		qq.from_axis_angle(Vector3f(.0f, .0f, 1.0f), -2.5f + _timeStamp / 20.0f / 1.0e6f);
//
////		double t = (double)(-1.5f + _timeStamp / 20.0f / 1.0e6f) * 57.3;
//
//		Dcmf RR(qq);
//
//		target_pos = RR * Vector3f(.4f, 0.0f, 0.0f)
//				+ MANI_OFFSET + MANI_FIRST_JOINT;


//		Vector3f target_vel(_target_sub.get().vx, _target_sub.get().vy, _target_sub.get().vz);


		print_info(print, &mavlink_log_pub, "target x:%8.4f, y:%8.4f, z:%8.4f",
			(double)target_pos(0),
			(double)target_pos(1),
			(double)target_pos(2));

		Matrix3f R_att(_att_sub.get().R);
		Vector3f distance = R_att.transpose() * (target_pos - pos) - MANI_OFFSET - MANI_FIRST_JOINT;

		Vector3f mani_sp(distance);
		print_info(print, &mavlink_log_pub, "distance x:%8.4f, y:%8.4f, z:%8.4f",
				(double)distance(0),
				(double)distance(1),
				(double)distance(2));

		_in_range = 0;
		/*when target is in range between two  -bdai<16 Nov 2016>*/
		float distance_norm = distance.norm();
		if (distance_norm < MANI_RANGE[R_MIN]) {
			mani_sp = MANI_RANGE[R_MIN] * mani_sp.normalized();
		} else if (distance_norm > MANI_RANGE[R_MAX]) {
			mani_sp = MANI_RANGE[R_MAX] * mani_sp.normalized();
		}
		else {
			_in_range |= 1;
		}

		Vector3f projection_normlized = Vector3f(mani_sp(0), mani_sp(1), 0.0f).normalized();
		float mani_sp_norm = mani_sp.norm();

//		float sin_theta = (Vector3f(mani_sp(0), mani_sp(1), 0).normalized() % mani_sp.normalized()).norm();
		Vector3f mani_sp_nomolized = mani_sp.normalized();
		float sin_theta = -mani_sp_nomolized(2);

		if ( sin_theta < sinf(MANI_RANGE[THETA_MIN])) {
			Vector3f r = (mani_sp % projection_normlized).normalized();
			Quatf q;
			q.from_axis_angle(r, MANI_RANGE[THETA_MIN]);
			Dcmf R(q);
			mani_sp = R * projection_normlized * mani_sp_norm;
		} else if(sin_theta > sinf(MANI_RANGE[THETA_MAX])) {
			Vector3f r = (mani_sp % projection_normlized).normalized();
			Quatf q;
			q.from_axis_angle(r, MANI_RANGE[THETA_MAX]);
			Dcmf R(q);
			mani_sp = R * projection_normlized * mani_sp_norm;
		} else {
			_in_range |= 1<<1;
		}

		float sin_psi = projection_normlized(1);
		mani_sp_norm = mani_sp.norm();
		Vector3f rotate_to_x = Vector3f(sqrt(mani_sp_norm*mani_sp_norm - mani_sp(2)*mani_sp(2)), 0, mani_sp(2));

		if ( sin_psi < sinf(MANI_RANGE[PSI_MIN])) {
			Quatf q(cosf(MANI_RANGE[PSI_MIN] / 2.0f), 0.0f, 0.0f, sinf(MANI_RANGE[PSI_MIN] / 2.0f));
			Dcmf R(q);
			mani_sp = R * rotate_to_x;
		} else if(sin_psi > sinf(MANI_RANGE[PSI_MAX])) {
			Quatf q(cosf(MANI_RANGE[PSI_MAX] / 2.0f), 0.0f, 0.0f, sinf(MANI_RANGE[PSI_MAX] / 2.0f));
			Dcmf R(q);
			mani_sp = R * rotate_to_x;
		} else {
			_in_range |= 1<<2;
		}

		mani_sp = mani_sp + MANI_FIRST_JOINT;

		print_info(print, &mavlink_log_pub, "_in_range: %d", _in_range);
		if (_in_range == 7){
			if (!_mani_triggered){
				_mani_triggered = true;	/*means manipulator can move now -bdai<13 Nov 2016>*/
			}

			float mani_error = Vector3f(mani_sp(0) - _mani_status_sub.get().x,
					mani_sp(1) - _mani_status_sub.get().y,
					mani_sp(2) - _mani_status_sub.get().z).norm();
//			mani_error = 0.01f;

			if(!_relative_rest) {
				if (mani_error < ACCURACY)
				{
					_relative_rest_time = _timeStamp;
					_relative_rest = true;
				} else {
					_relative_rest = false;
				}
			}
		} else {
			_relative_rest = false;
			_relative_rest_time = _timeStamp;
		}


		_manip_pub.get().x= mani_sp(0);
		_manip_pub.get().y= mani_sp(1);
		_manip_pub.get().z= mani_sp(2);

		/*calculate grabber velosity -bdai<19 Nov 2016>*/
		{
			Vector3f mani_status(_mani_status_sub.get().x,
				_mani_status_sub.get().y, _mani_status_sub.get().z);

			Vector3f mani_vel  = MANI_P * (mani_sp - mani_status);

			if (mani_vel.norm() > MANI_MAX_VEL){
				mani_vel = mani_vel.normalized() * MANI_MAX_VEL;
			}

			_manip_pub.get().vx = mani_vel(0);
			_manip_pub.get().vy = mani_vel(1);
			_manip_pub.get().vz = mani_vel(2);

		}

		/*calculate grabber euler angle -bdai<17 Nov 2016>*/
		{
			Vector3f R_z(-distance.normalized());
			Vector3f R_x = (Vector3f(.0f, .0f, 1.0f) % R_z).normalized();
			if (R_z(2) < sinf(GRAPPER_ANGLE_RANGE[0])) {
				Quatf q;
				q.from_axis_angle(R_x, (float)M_PI / 2.0f - GRAPPER_ANGLE_RANGE[0]);
				Dcmf R(q);
				R_z = R * Vector3f(.0f, .0f, 1.0f);
			} else if (R_z(2) > sinf(GRAPPER_ANGLE_RANGE[1])) {
				Quatf q;
				q.from_axis_angle(R_x, (float)M_PI / 2.0f - GRAPPER_ANGLE_RANGE[1]);
				Dcmf R(q);
				R_z = R * Vector3f(.0f, .0f, 1.0f);
			}

			Vector3f R_y = (R_z % R_x).normalized();

			Dcmf R;
			R.setCol(0, R_x);
			R.setCol(1, R_y);
			R.setCol(2, R_z);

			Eulerf euler(R);
			_manip_pub.get().roll = euler.phi();
			_manip_pub.get().pitch = euler.theta();
			_manip_pub.get().yaw = euler.psi();

		}

		/*hold on in init position -bdai<13 Nov 2016>*/
		if (!_mani_triggered) {
			mani_init_position();
		}

		if (_relative_rest && (_timeStamp - _relative_rest_time) > REST_DURATION)
		{
			_manip_pub.get().arm_enable = 1;
		}

		//grab success
		if (_mani_status_sub.get().gripper_status == -1) {
//			_manip_pub.get().z = _manip_pub.get().z - 0.05f;
			mani_init_position();
			_manip_pub.get().arm_enable = 1;
			_manip_pub.update();
			_manip_pub.update();
//			_grabbed = true;
			_grabbed = false;
		}

	} else {
		_mani_triggered = false;
		_grabbed = false;
		mani_init_position();
	}
	_manip_pub.get().timestamp = _timeStamp;

	if (!_grabbed) {
		_manip_pub.update();
	}

	print_info(print, &mavlink_log_pub, "x %8.4f, y %8.4f, z %8.4f, roll %8.4f, pitch %8.4f, yaw %8.4f, arm_enable %d",
				(double)_manip_pub.get().x,
				(double)_manip_pub.get().y,
				(double)_manip_pub.get().z,
				(double)_manip_pub.get().roll,
				(double)_manip_pub.get().pitch,
				(double)_manip_pub.get().yaw,
				_manip_pub.get().arm_enable);
}

void BlockManipulatorControl::mani_init_position()
{
	_manip_pub.get().x = .0f;
	_manip_pub.get().y = .0f;
	_manip_pub.get().z = .0f;
	_manip_pub.get().vx = .0f;
	_manip_pub.get().vy = .0f;
	_manip_pub.get().vz = .0f;
	_manip_pub.get().arm_enable = 0;
}








