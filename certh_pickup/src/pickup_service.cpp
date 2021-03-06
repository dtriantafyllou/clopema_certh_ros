#include "PickUp.h"

#include <ros/ros.h>
#include <robot_helpers/Robot.h>
#include <certh_pickup/PickUp.h>

bool do_pickup(certh_pickup::PickUp::Request &req,   certh_pickup::PickUp::Response &res)
{
    PickUp pick("r2");

    ros::Duration wait = ( req.time_out > 0 ) ? ros::Duration(req.time_out) : ros::Duration(100) ;

    if ( pick.graspClothFromTable(wait) )
        res.status = 0 ;
    else
        res.status = 1 ;

    setServoPowerOff();

    return true ;
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, "pick_cloth_from_table_service");

    ros::NodeHandle nh("~");

    // Register the service with the master
    ros::ServiceServer server = nh.advertiseService("pickup", &do_pickup);

    ros::spin() ;

}
