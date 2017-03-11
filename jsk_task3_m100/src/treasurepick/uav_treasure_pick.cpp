/*
 jsk_mbzirc_task
 */

// Author: Chen

#include <ros/ros.h>

//msg headers.
#include <geometry_msgs/PoseArray.h>
#include <geometry_msgs/Pose.h>
#include <geometry_msgs/Twist.h>
#include <nav_msgs/Odometry.h>
#include <std_msgs/Int16.h>
#include <std_msgs/Float32.h>
#include <sensor_msgs/LaserScan.h>
//sys lib
#include <iostream>
#include <string>
#include <stdlib.h>
//srv
#include <jsk_mbzirc_board/Magnet.h>
//#include <gazebo_msgs/GetModelState.h>
//boost
#include <boost/random.hpp>
#include <boost/random/random_device.hpp>
//pcl
#include <ctime>
//the store the detected objects
//dji sdk
#include <dji_sdk/dji_sdk.h>
#include <dji_sdk/dji_drone.h>
#include <Eigen/Core>
#include <tf/transform_broadcaster.h>
typedef struct
{
    geometry_msgs::Pose pose;
    //if this is large enough, publish the pose
    //if is minus too small, remove it from the vector
    int visible_times;
}Treasure;

class treasure_pick
{

private:

    ros::NodeHandle nh_;
    //subscriber
    ros::Subscriber object_pose_sub_;
    ros::Subscriber uav_odom_sub_;
    ros::Subscriber mag_feedback_sub_;
    ros::Subscriber guidance_sub_;
    sensor_msgs::LaserScan ultra_sonic;
    ros::Subscriber lidar_sub_;
    float lidar_data;
    ros::Subscriber share_odom_sub_;  //self global position
    //publisher
    ros::Publisher aim_pose_pub_;
    ros::Publisher mag_pub_;
    //data field
    geometry_msgs::Pose aim_pose;
    geometry_msgs::Pose box_pose;
    geometry_msgs::Pose search_pose;
    std_msgs::Float32 magnet_state;// how many switches are on
    int switch_on_counter;
    //srv
    ros::ServiceClient mag_srv_;
    jsk_mbzirc_board::Magnet mag_srv_status;

    std_msgs::Int16 mag_on;
    nav_msgs::Odometry uav_odom;
    nav_msgs::Odometry global_odom;
    std::vector<Treasure> treasure_vec;

    const double distthreshold = 1; //here we assume that no object is near with 1 meter

    enum State_Machine{Picking, Placing, Searching}uav_task_state;
    // drone name
    DJIDrone* DJI_M100;
    
    double vel_nav_gain_;
    double vel_nav_limit_;

    //dirty flag....
    int attemp_to_back = 0;
    int attemp_time = 0;
    int canseecounter = 0;


public:
    void init()
    {
        //DJI SDK
//        DJI_M100 = new DJIDrone(nh_);
       //subscriber
        object_pose_sub_ = nh_.subscribe("/obj_cluster/centroid_pose",
                                         1,&treasure_pick::ObjPoseCallback,this);
        uav_odom_sub_ = nh_.subscribe("/dji_sdk/odometry",1,&treasure_pick::OdomCallback,this);
        mag_feedback_sub_ = nh_.subscribe("/magnet_feedback",1,&treasure_pick::PickCallback,this);
        aim_pose_pub_ = nh_.advertise<geometry_msgs::Pose>("aimpose",1);
        guidance_sub_ = nh_.subscribe("/guidance/ultrasonic", 1, &treasure_pick::UltraSonicCallback, this);
        lidar_sub_ = nh_.subscribe<std_msgs::Float32>("/lidar_laser",1,&treasure_pick::LidarCallback,this);
        //
        share_odom_sub_ = nh_.subscribe("/share_odometry",10,&treasure_pick::ShareOdomCallback,this);
	//srv
        mag_srv_ = nh_.serviceClient<jsk_mbzirc_board::Magnet>("serial_board/magnet_control");
        mag_srv_status.request.on = false;
        mag_srv_status.request.time_ms = 2000; //3 second

        magnet_state.data = 0;
        switch_on_counter = 0;
        //box pose, when picked, go to the box...
        box_pose.position.x = 0;
        box_pose.position.y = 0;
        box_pose.position.z = 1.8;
        search_pose.position.z = 5; //5 meters high...
        aim_pose = search_pose;
        uav_task_state = Searching;

	vel_nav_limit_ = 0.4;
	vel_nav_gain_ = 1.0;
    }
    inline bool DistLessThanThre(geometry_msgs::Point P1, geometry_msgs::Point P2, double threshold)
    {
        double dist = (P1.x-P2.x)*(P1.x-P2.x)+(P1.y-P2.y)*(P1.y-P2.y)+
                (P1.z-P2.z)*(P1.z-P2.z);
        if(dist>(threshold*threshold))
            return false;
        else
            return true;
    }

    //renew the data of the gazebo objects
    void ObjPoseCallback(const geometry_msgs::PoseArray posearray)
    {
        //update the aim_pose
        if(uav_task_state==Placing)
          return;

        if(!posearray.poses.size())
          return;

        if(!treasure_vec.size())
        {
            Treasure tmp_t;
            tmp_t.pose = posearray.poses.at(0);
            tmp_t.visible_times = 0;
            treasure_vec.push_back(tmp_t);
        }
        for(int i = 0; i < posearray.poses.size(); i++)
        {
            for(int j = 0; j < treasure_vec.size(); j++)
            {
                treasure_vec.at(j).visible_times -= 1; //every object minus 1
                if(DistLessThanThre(treasure_vec.at(j).pose.position
                                    ,posearray.poses.at(i).position
                                    ,distthreshold)
                        &&(treasure_vec.at(j).pose.orientation.y ==
                           posearray.poses.at(i).orientation.y))
                {
                    treasure_vec.at(j).visible_times += 8; //plus 5
                    //here we also need to consider the size of the detected points
                    // if 400 points are detected then plus 4
                    treasure_vec.at(j).visible_times +=  posearray.poses.at(i).orientation.x/100;
                    //if they are the same one, update the pose
                    treasure_vec.at(j).pose = posearray.poses.at(i);
                    treasure_vec.at(j).visible_times
                            = treasure_vec.at(j).visible_times>
                            50?50:treasure_vec.at(j).visible_times;
                    break;
                }
                //larger than threshold
                else
                {
                    if(j == treasure_vec.size()-1)//the last one
                    {
                        Treasure tmp_t;
                        tmp_t.pose = posearray.poses.at(i);
                        tmp_t.visible_times = 0;
                        treasure_vec.push_back(tmp_t);
                        break;
                    }
                }
                //remove the one that can't be seen for more than 15 times.
                if(treasure_vec.at(j).visible_times < -20)
                    treasure_vec.erase(treasure_vec.begin() + j);
                //remove the one near the box
                //-65,25,
//                if((treasure_vec.at(j).pose.position.x>-70&&
//                        treasure_vec.at(j).pose.position.x<-60&&
//                        treasure_vec.at(j).pose.position.y>20&&
//                        treasure_vec.at(j).pose.position.y<30)
//                        ||(treasure_vec.at(j).pose.position.y>-0.1&&
//                        treasure_vec.at(j).pose.position.y<0.1))
//                    treasure_vec.erase(treasure_vec.begin() + j);
            }
        }
        //publish the pose, maybe we should use the same one, now use the first one
        for(int i=0;i<treasure_vec.size();i++)
        {
            if(treasure_vec.at(i).visible_times>35)
            {
                aim_pose = treasure_vec.at(i).pose;
                //aim_pose_pub_.publish(aim_pose);
                uav_task_state = Picking;
                break;
            }
        }
    }
    /***********
     * global call back
     * *********/
   void ShareOdomCallback(const nav_msgs::Odometry odom)
   {
       global_odom = odom;
   }
    /***********
     * odometry call back
     * *********/
    void OdomCallback(const nav_msgs::Odometry odom)
    {
        double uav_h;
        if(odom.pose.pose.position.z<1.5&&ultra_sonic.ranges.size()&&ultra_sonic.ranges.at(0)>0.05) //less than 1 meter
            uav_h = ultra_sonic.ranges.at(0) - 0.05 ;
        else if(odom.pose.pose.position.z<1&&lidar_data>0&&lidar_data<2) //less than 1 meter
            uav_h = lidar_data;
        else
	  uav_h = odom.pose.pose.position.z;
	//test guidance sucks...
	//	uav_h = odom.pose.pose.position.z;

        uav_odom = odom; //update
        //publish by status of the state machine
        //can be set to a fixed frequency..
        if(uav_task_state==Placing)\
          {
            aim_pose_pub_.publish(box_pose);
            //check if the location is near and then publish mag false
            //and the speed is very low
            if((fabs(global_odom.pose.pose.position.x-box_pose.position.x)<0.15)&&
               (fabs(global_odom.pose.pose.position.y-box_pose.position.y)<0.15)&&
               (fabs(global_odom.pose.pose.position.z-box_pose.position.z)<0.3))
              {
                //here to release the magnets
                if(mag_srv_.call(mag_srv_status))
                    ROS_INFO("Gripper Released for 1000 ms");
                else
                    ROS_INFO("Fail to release the gripper");

                aim_pose = search_pose;
                ROS_WARN("left treasure at %f,%f,%f",
                         uav_odom.pose.pose.position.x,
                         uav_odom.pose.pose.position.y,
                         uav_odom.pose.pose.position.z);
              }
          }
        else if(uav_task_state==Searching)
          {

           /*  now we need to using the global odom for searching...
            * We record 16 GPS points for each UAV, each uav go from
            * GPS point 1 - 16....   when it reaches the tempory one, go next..
            *
           */
            if((fabs(global_odom.pose.pose.position.x-search_pose.position.x)<0.5)&&
               (fabs(global_odom.pose.pose.position.y-search_pose.position.y)<0.5)&&
               (fabs(global_odom.pose.pose.position.z-search_pose.position.z)<0.5))
              {
               // update the searching pose from the recorded gps points.

                             //TBD!!!!!!!!!!!!!!!!!!!!

                ROS_WARN("Send random search place at: %f,%f,%f",
                         search_pose.position.x,
                         search_pose.position.y,
                         search_pose.position.z
                         );
              }
            //we have already set aim_pose to the search pose.
            //if the detector renew the aim_pose, we will pick...

            aim_pose.orientation.w = 1; //this means to control global frame...
            //transfer the global frame to the
            aim_pose.position.x = search_pose.position.x - global_odom.pose.pose.position.x;
            aim_pose.position.y = search_pose.position.y - global_odom.pose.pose.position.y;
            aim_pose.position.z = 4; // search pose always to be 4 meters
            aim_pose_pub_.publish(aim_pose);
          }
        else
          {
            //picking
            //consider pick failure approach for ten times....

            if(uav_h < 0.7)  //less than 0.8 meter, do pick attemp...
                if(uav_h < 0.3)
                {
                    if(attemp_to_back == 2)
                        attemp_time ++;
                    attemp_to_back = 3;
                    aim_pose.orientation.w = 3; // 3 means going back to 3 mete
		    std::cout<<"now I am trying to hold back to 3 meters.."<<std::endl;
                }
                else
                {
                    attemp_to_back = 2;
                    aim_pose.orientation.w = 2; // 2 means making pick attemp..
		    std::cout<<"I am tring to pick"<<std::endl;
                }

            if(uav_h > 2)
            {
                canseecounter++;
                attemp_to_back = 0; // above 2 meters then enable approach
                if(canseecounter > 1000) //500 means ten seconds
                    uav_task_state = Searching;
            }
            else
            {
                canseecounter = 0;
            }

            if(attemp_time > 10)
            {

               attemp_time++;
               if(attemp_time > 100)  // let it go a little bit far away and try....
               {
                   attemp_time = 0;
                   uav_task_state = Searching;
		   std::cout<<"back to searching mode"<<std::endl;
               }
               aim_pose = search_pose;
            }
	    std::cout<<"the height is " << uav_h <<std::endl;
            aim_pose_pub_.publish(aim_pose);

//	    DJI_M100->local_position_control(aim_pose.position.x,aim_pose.position.y,aim_z,0);
//        std::cout<<"control drone to go:"<< aim_pose.position.x<<aim_pose.position.y<<aim_z<<std::endl;
          }
    }

    void PickCallback(const std_msgs::Float32 pickstate)
    {


        if(!magnet_state.data&&pickstate.data) //from false to true, swtich from 0 to 1234(on)
        {
            switch_on_counter++;
            uav_task_state = Placing;
        }
        else if(magnet_state.data&&!pickstate.data) //frome true to false, swtich from 1234 to 0(release)
        {
            switch_on_counter = 0;
            uav_task_state = Searching;
            aim_pose = search_pose;
            treasure_vec.clear();
        }
        magnet_state = pickstate;
    }
    void LidarCallback(const std_msgs::Float32 data)
    {
        lidar_data = (float)data.data;
        lidar_data /= 100; //from cm to meter
    }
	
    void UltraSonicCallback(const sensor_msgs::LaserScan data)
    {
        if(data.ranges.size())
            if(data.ranges.at(0)<0.39 || data.ranges.at(0)>0.41)
                ultra_sonic = data; //update ultra sonic...
    }

    ~treasure_pick()
    {    }
};


int main(int argc, char **argv)
{
    ros::init(argc, argv, "uav_treasure_pick");
    treasure_pick t_p;
    t_p.init();

    ros::spin();

}
