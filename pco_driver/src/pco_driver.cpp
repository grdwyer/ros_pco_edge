//
// Created by george on 10/6/21.
//

#include <pco_driver/pco_driver.h>

static const rclcpp::Logger LOGGER = rclcpp::get_logger("pco_camera_driver");

PCODriver::PCODriver(const rclcpp::NodeOptions &options) : Node("pco_camera_driver", options){
    // Declare any parameters needed
    this->declare_parameter<std::string>("frame_id", "pco_camera");
    this->declare_parameter<std::string>("camera_calibration_file", "file://config/camera.yaml");
    this->declare_parameter<int>("desired_framerate", 10);
    this->declare_parameter<int>("camera_id", 0);

    // Initialise ROS objects
    rmw_qos_profile_t custom_qos_profile = rmw_qos_profile_sensor_data;
    camera_publisher_ = image_transport::create_camera_publisher(this, this->get_fully_qualified_name() + std::string("/image"));

    camera_info_manager_ = std::make_shared<camera_info_manager::CameraInfoManager>(this);
    camera_info_manager_->loadCameraInfo(this->get_parameter("camera_calibration_file").as_string());

    if(!initialiseCamera()){
        RCLCPP_ERROR_STREAM(LOGGER, "Failed to initialise camera");
        return;
    }
}

bool PCODriver::initialiseCamera() {
    int board = this->get_parameter("camera_id").as_int();
    RCLCPP_INFO_STREAM(LOGGER, "Starting up camera on board " << board);
    pco_camera_ = std::make_shared<CPco_com_usb>();
    pco_error_ = pco_camera_->Open_Cam((WORD)board);
    if(pco_error_!=PCO_NOERROR)
    {
        RCLCPP_ERROR_STREAM(LOGGER, "Failed to open camera with ERROR: \n" << getPCOError(pco_error_) << "\n\nExiting\n");
        return false;
    }

    RCLCPP_INFO_STREAM(LOGGER, "Starting up grabber ");
    pco_grabber_ = std::make_shared<CPco_grab_usb>((CPco_com_usb*)pco_camera_.get());
    pco_error_ = pco_grabber_->Open_Grabber(board);
    if(pco_error_!=PCO_NOERROR)
    {
        RCLCPP_ERROR_STREAM(LOGGER, "Failed to open grabber with ERROR: \n" << getPCOError(pco_error_) << "\n\nExiting\n");
        return false;
    }

    pco_error_ = pco_grabber_->Set_Grabber_Timeout(10000);
    if(pco_error_!=PCO_NOERROR)
    {
        RCLCPP_ERROR_STREAM(LOGGER, "Failed to set grabber timeout with ERROR: \n" << getPCOError(pco_error_) << "\n\nExiting\n");
        return false;
    }

    pco_error_ = pco_camera_->PCO_SetCameraToCurrentTime();
    if(pco_error_!=PCO_NOERROR)
    {
        RCLCPP_ERROR_STREAM(LOGGER, "Failed to set camera to current time with ERROR: \n" << getPCOError(pco_error_) << "\n\nExiting\n");
        return false;
    }
    
    pco_error_ = pco_camera_->PCO_SetRecordingState(0);
    if(pco_error_!=PCO_NOERROR)
    {
        RCLCPP_ERROR_STREAM(LOGGER, "Failed to set camera state to stop with ERROR: \n" << getPCOError(pco_error_) << "\n\nExiting\n");
        return false;
    }

    pco_error_ = pco_camera_->PCO_ResetSettingsToDefault();
    if(pco_error_!=PCO_NOERROR)
    {
        RCLCPP_ERROR_STREAM(LOGGER, "Failed to reset the settings to default ERROR: \n" << getPCOError(pco_error_) << "\n\nExiting\n");
        return false;
    }

    pco_error_ = pco_camera_->PCO_SetTimestampMode(2);
    if(pco_error_!=PCO_NOERROR)
    {
        RCLCPP_ERROR_STREAM(LOGGER, "Failed to set timestamp mode of the camera with ERROR: \n" << getPCOError(pco_error_) << "\n\nExiting\n");
        return false;
    }

    pco_error_ = pco_camera_->PCO_SetTimebase(1, 2);
    if(pco_error_!=PCO_NOERROR)
    {
        RCLCPP_ERROR_STREAM(LOGGER, "Failed to set timebase of the camera with ERROR: \n" << getPCOError(pco_error_) << "\n\nExiting\n");
        return false;
    }

    pco_error_ = pco_camera_->PCO_SetDelayExposure(0, 40);
    if(pco_error_!=PCO_NOERROR)
    {
        RCLCPP_ERROR_STREAM(LOGGER, "Failed to set the delay and exposure time of the camera with ERROR: \n" << getPCOError(pco_error_) << "\n\nExiting\n");
        return false;
    }

    RCLCPP_INFO_STREAM(LOGGER, "Determining image configuration");
    pco_error_ = pco_camera_->PCO_GetActualSize(&image_width_, &image_height_);
    if(pco_error_!=PCO_NOERROR)
    {
        RCLCPP_ERROR_STREAM(LOGGER, "Failed to determine image size with ERROR: \n" << getPCOError(pco_error_) << "\n\nExiting\n");
        return false;
    }

    WORD bit_alignment;
    pco_error_ = pco_camera_->PCO_GetBitAlignment(&bit_alignment); // 0 is MSB (big endian), 1 is LSB (little endian)

    RCLCPP_INFO_STREAM(LOGGER, "Image width: " << image_width_ << " Image height: " << image_height_);

    // Setup the buffer for the images
    pco_buffer_.assign(image_height_ * image_width_, 0);
    image_msg_ = std::make_shared<sensor_msgs::msg::Image>();
    image_msg_->data.assign(2 * image_height_ * image_width_, 0); // 16 bit stored in 8 bit so times by 2
    image_msg_->width = image_width_;
    image_msg_->height = image_height_;
    image_msg_->step = 2 * image_width_;   //row length in bytes as each is a word (uint16)
    image_msg_->is_bigendian = bit_alignment == 0;
    image_msg_->encoding = sensor_msgs::image_encodings::MONO16;
    image_msg_->header.frame_id = this->get_parameter("frame_id").as_string();

    RCLCPP_INFO_STREAM(LOGGER, "Preparing camera to record");
    pco_error_ = pco_camera_->PCO_ArmCamera();
    if(pco_error_!=PCO_NOERROR)
    {
        RCLCPP_ERROR_STREAM(LOGGER, "Failed to arm the camera ERROR: \n" << getPCOError(pco_error_) << "\n\nExiting\n");
        return false;
    }
    pco_error_ = pco_grabber_->PostArm();
    if(pco_error_!=PCO_NOERROR)
    {
        RCLCPP_ERROR_STREAM(LOGGER, "Failed in the post arming setup of the grabber ERROR: \n" << getPCOError(pco_error_) << "\n\nExiting\n");
        return false;
    }

    pco_error_ = pco_camera_->PCO_SetRecordingState(1);
    if(pco_error_!=PCO_NOERROR)
    {
        RCLCPP_ERROR_STREAM(LOGGER, "Failed to set the camera to record ERROR: \n" << getPCOError(pco_error_) << "\n\nExiting\n");
        return false;
    }

    auto duration = std::chrono::milliseconds(int(1.0/ this->get_parameter("desired_framerate").as_int() * 1000));
    timer_ = this->create_wall_timer(duration, std::bind(&PCODriver::imageCallback, this));
    RCLCPP_INFO_STREAM(LOGGER, "Camera is now recording");

    return true;
}

uint8_t convert_to_uint(const WORD a){
//    uint8_t Test2 = (unsigned)(Test >> 4);
    return (unsigned)(a >> 4);
}

void PCODriver::imageCallback() {
    // Get latest image from camera/grabber
    auto& clk = *this->get_clock();
    RCLCPP_DEBUG_THROTTLE(LOGGER,
                         clk,
                         5000,"Image callback ");
    pco_error_ = pco_grabber_->Acquire_Image(pco_buffer_.data());
    //convert to ros image
    // Clear has a lot of overhead should just overwrite by index probably.
    image_msg_->data.clear();
    for(const auto value : pco_buffer_){
        image_msg_->data.push_back((uint8_t) value);
        image_msg_->data.push_back((uint8_t )(value >> 8));
    }

// // Should work but not currently working.
//    for(size_t i = 0; i < pco_buffer_.size(); i++){
//        // The order will need to change if it is meant to be big endian
//        image_msg_->data[2*i] = (uint8_t) pco_buffer_[i]; // lo
//        image_msg_->data[(2*i)+1] = (uint8_t) pco_buffer_[i] >> 8; //hi
//    }
    //publish with the camera info
    sensor_msgs::msg::CameraInfo::SharedPtr camera_info_msg_(
            new sensor_msgs::msg::CameraInfo(camera_info_manager_->getCameraInfo()));

    rclcpp::Time timestamp = this->get_clock()->now();

    image_msg_->header.stamp = timestamp;
    image_msg_->header.frame_id = this->get_parameter("frame_id").as_string();

    camera_info_msg_->header.stamp = timestamp;
    camera_info_msg_->header.frame_id = this->get_parameter("frame_id").as_string();

    camera_publisher_.publish(image_msg_, camera_info_msg_);

}

PCODriver::~PCODriver() {
    RCLCPP_WARN_STREAM(LOGGER,"PCO driver is shutting down");
    timer_->cancel();
    camera_publisher_.shutdown();
    pco_grabber_->Close_Grabber();
    pco_camera_->Close_Cam();

}

std::string PCODriver::getPCOError(WORD error_code) {
    char pco_error_text[500];
    PCO_GetErrorText(error_code, pco_error_text, 500);
    return std::string(pco_error_text);
}


#include "rclcpp_components/register_node_macro.hpp"

// Register the component with class_loader.
// This acts as a sort of entry point, allowing the component to be discoverable when its library
// is being loaded into a running process.
RCLCPP_COMPONENTS_REGISTER_NODE(PCODriver)
