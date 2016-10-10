// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2015 Intel Corporation. All Rights Reserved.

#include <iostream>
#include <iomanip>
#include "types.h"
#include "hw-monitor.h"
#include "ds-private.h"
#include "ds5-private.h"

#define stringify( name ) # name "\t"

using namespace rsimpl::hw_monitor;
using namespace rsimpl::ds5;

namespace rsimpl {
namespace ds5 {

    const uvc::extension_unit depth_xu = { 0, 3, 2, { 0xC9606CCB, 0x594C, 0x4D25,{ 0xaf, 0x47, 0xcc, 0xc4, 0x96, 0x43, 0x59, 0x95 } } };
    const uvc::guid DS5_WIN_USB_DEVICE_GUID = { 0x08090549, 0xCE78, 0x41DC,{ 0xA0, 0xFB, 0x1B, 0xD6, 0x66, 0x94, 0xBB, 0x0C } };

    enum fw_cmd : uint8_t
    {
        GVD         = 0x10,
        GETINTCAL   = 0x15,     // Read calibration table
    };

#pragma pack(push, 1)

    struct table_header
    {
        big_endian<uint16_t>    version;        // major.minor. Big-endian
        uint16_t                table_type;     // ctCalibration
        uint32_t                table_size;     // full size including: TOC header + TOC + actual tables
        uint32_t                param;          // This field content is defined ny table type
        uint32_t                crc32;          // crc of all the actual table data excluding header/CRC
    };

    struct table_link
    {
        uint16_t        table_id;       // enumerated table id
        uint16_t        param;          // This field is determined uniquely by each table
        uint32_t        offset;         // table actual location offset (address) in the flash memory
    };

    struct eprom_table
    {
        table_header    header;
        table_link      optical_module_info;   // enumerated table id
        table_link      calib_table_info;      // index of the depth camera in case of multiple instances
    };

    /* Used for iterating through internal TOC stored on the flash*/
    enum data_tables
    {
        depth_calibration_a,
        depth_coefficients_a,
        rgb_calibration,
        fisheye_calibration,
        imu_calibration,
        lens_shading,
        projector,
        max_ds5_intrinsic_table
    };

    struct calibrations_header
    {
        table_header        toc_header;
        table_link          toc[max_ds5_intrinsic_table];   // list of all data tables stored on flash
    };

    enum m3_3_field
    {
        f_1_1           = 0,
        f_1_2           = 1,
        f_1_3           = 2,
        f_2_1           = 3,
        f_2_2           = 4,
        f_2_3           = 5,
        f_3_1           = 6,
        f_3_2           = 7,
        f_3_3           = 8,
        f_x             = 0,
        f_y             = 1,
        p_x             = 2,
        p_y             = 3,
        distortion_0    = 4,
        distortion_1    = 5,
        distortion_2    = 6,
        distortion_3    = 7,
        distortion_4    = 8,
        max_m3_3_field  = 9
    };

    struct coefficients_table
    {
        table_header        header;
        float3x3            intrinsic_left;             //  left camera intrinsic data, normilized
        float3x3            intrinsic_right;            //  right camera intrinsic data, normilized
        float3x3            world2left_rot;             //  the inverse rotation of the left camera
        float3x3            world2right_rot;            //  the inverse rotation of the right camera
        float               baseline;                   //  the baseline between the cameras
        uint8_t             reserved1[88];
        uint32_t            brown_model;                // 0 - using DS distorion model, 1 - using Brown model
        float4              rect_params[max_ds5_rect_resoluitons];
        uint8_t             reserved2[64];
    };

    struct depth_calibration_table
    {
        table_header        header;
        float               r_max;                      //  the maximum depth value in mm corresponding to 65535
        float3x3            k_left;                     //  Left intrinsic matrix, normalize by [-1 1]
        float               distortion_left[5];         //  Left forward distortion parameters, brown model
        float3              r_left;                     //  Left rotation angles (Rodrigues)
        float3              t_left;                     //  Left translation vector, mm
        float3x3            k_right;                    //  Right intrinsic matrix, normalize by [-1 1]
        float               distortion_right[5];        //  Right forward distortion parameters, brown model
        float3              r_right;                    //  Right rotation angles (Rodrigues)
        float3              t_right;                    //  Right translation vector, mm
        float3x3            k_depth;                    //  the Depth camera intrinsic matrix
        float               r_depth[3];                 //  Depth rotation angles (Rodrigues)
        float               t_depth[3];                 //  Depth translation vector, mm
        unsigned char       reserved[16];
    };
#pragma pack(pop)

    const uint8_t DS5_MONITOR_INTERFACE                 = 0x3;
    const uint8_t DS5_MOTION_MODULE_INTERRUPT_INTERFACE = 0x4;


    void claim_ds5_monitor_interface(uvc::device & device)
    {
        claim_interface(device, DS5_WIN_USB_DEVICE_GUID, DS5_MONITOR_INTERFACE);
    }

    void claim_ds5_motion_module_interface(uvc::device & device)
    {
        const uvc::guid MOTION_MODULE_USB_DEVICE_GUID = {};
        claim_aux_interface(device, MOTION_MODULE_USB_DEVICE_GUID, DS5_MOTION_MODULE_INTERRUPT_INTERFACE);
    }

    // "Get Version and Date"
    void get_gvd_raw(uvc::device & device, std::timed_mutex & mutex, size_t sz, unsigned char * gvd)
    {
        hwmon_cmd cmd(fw_cmd::GVD);
        perform_and_send_monitor_command_over_usb_monitor(device, mutex, cmd);

        if (cmd.receivedCommandDataLength > sz)
            LOG_WARNING("received command data length is greater than ds5 gvd_size");

        auto min_size = std::min(sz, cmd.receivedCommandDataLength);
        memcpy(gvd, cmd.receivedCommandData, min_size);
    }

    void get_string_of_gvd_field(uvc::device & device, std::timed_mutex & mutex, std::string & str, gvd_offset_fields offset)
    {
        if (offset > gvd_size)
            throw std::logic_error("offset can't be greater than ds5 gvd_size");

        std::vector<unsigned char> gvd(gvd_size);
        get_gvd_raw(device, mutex, gvd_size, gvd.data());
        unsigned char* gvd_pointer = gvd.data() + offset;
        std::stringstream ss;

        switch (offset)
        {
        case gvd_offset_fields::asic_module_serial_offset:
            ss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(gvd_pointer[0]) <<
                              std::setfill('0') << std::setw(2) << static_cast<int>(gvd_pointer[1]) <<
                              std::setfill('0') << std::setw(2) << static_cast<int>(gvd_pointer[2]) <<
                              std::setfill('0') << std::setw(2) << static_cast<int>(gvd_pointer[3]) <<
                              std::setfill('0') << std::setw(2) << static_cast<int>(gvd_pointer[4]) <<
                              std::setfill('0') << std::setw(2) << static_cast<int>(gvd_pointer[5]);
            break;
        case gvd_offset_fields::fw_version_offset:
            ss << static_cast<int>(gvd_pointer[3]) << "." <<
                  static_cast<int>(gvd_pointer[2]) << "." <<
                  static_cast<int>(gvd_pointer[1]) << "." <<
                  static_cast<int>(gvd_pointer[0]);
            break;
        default:
            LOG_WARNING( "DS5 field at offset " << offset <<  " is not supported");
        }
        str = ss.str();
    }

    void get_calibration_table_entry(uvc::device & device, std::timed_mutex & mutex,calibration_table_id table_id, std::vector<uint8_t> & raw_data)
    {
        hwmon_cmd cmd(fw_cmd::GETINTCAL);
        cmd.Param1 = table_id;
        perform_and_send_monitor_command_over_usb_monitor(device, mutex, cmd);
        raw_data.resize(cmd.receivedCommandDataLength);
        raw_data.assign(cmd.receivedCommandData, cmd.receivedCommandData + cmd.receivedCommandDataLength);
    }

    template<typename T, int sz>
    int arr_size(T(&)[sz])
    {
        return sz;
    }

    template<typename T>
    const std::string array2str(T& data)
    {
        std::stringstream ss;
        for (int i = 0; i < arr_size(data); i++)
            ss << " [" << i << "] = " << data[i] << "\t";
        return std::string(ss.str().c_str());
    }

    typedef float float_9[9];
    typedef float float_4[4];

    void parse_calibration_table(ds5_calibration& calib, calibration_table_id table_id, std::vector<unsigned char> & raw_data)
    {
        switch (table_id)
        {
        case coefficients_table_id:
        {
            if (raw_data.size() != sizeof(coefficients_table))
            {
                std::stringstream ss; ss << "DS5 Coefficients table read error, actual size is " << raw_data.size() << " while expecting " << sizeof(coefficients_table) << " bytes";
                std::string str(ss.str().c_str());
                LOG_ERROR(str);
                throw std::runtime_error(str);
            }
            coefficients_table *table = reinterpret_cast<coefficients_table *>(raw_data.data());
            LOG_DEBUG("DS5 Coefficients table: version [mjr.mnr]: 0x" << std::hex  << std::setfill('0') << std::setw(4) << table->header.version << std::dec
                << ", type " << table->header.table_type          << ", size "    << table->header.table_size
                << ", CRC: " << std::hex << table->header.crc32);
            // verify the parsed table
            if (table->header.crc32 != calc_crc32(raw_data.data() + sizeof(table_header), raw_data.size() - sizeof(table_header)))
            {
                std::string str("DS5 Coefficients table CRC error, parsing aborted");
                LOG_ERROR(str);
                throw std::runtime_error(str);
            }

            LOG_DEBUG( std::endl
                << "baseline = " << table->baseline << " mm" << std::endl
                << "Rect params:  \t fX\t\t fY\t\t ppX\t\t ppY \n"
                << stringify(res_1920_1080) << array2str((float_4&)table->rect_params[res_1920_1080]) << std::endl
                << stringify(res_1280_720) << array2str((float_4&)table->rect_params[res_1280_720]) << std::endl
                << stringify(res_640_480) << array2str((float_4&)table->rect_params[res_640_480]) << std::endl
                << stringify(res_848_480) << array2str((float_4&)table->rect_params[res_848_480]) << std::endl
                << stringify(res_424_240) << array2str((float_4&)table->rect_params[res_424_240]) << std::endl
                << stringify(res_640_360) << array2str((float_4&)table->rect_params[res_640_360]) << std::endl
                << stringify(res_320_240) << array2str((float_4&)table->rect_params[res_320_240]) << std::endl
                << stringify(res_480_270) << array2str((float_4&)table->rect_params[res_480_270]));

            calib.left_imager_intrinsic.width       = -1; // applies for all resolutions
            calib.left_imager_intrinsic.height      = -1;

            calib.left_imager_intrinsic.fx          = table->intrinsic_left(0, 0);   // focal length of the image plane, as a multiple of pixel width
            calib.left_imager_intrinsic.fy          = table->intrinsic_left(0, 1);   // focal length of the image plane, as a multiple of pixel height
            calib.left_imager_intrinsic.ppx         = table->intrinsic_left(0, 2);   // horizontal coordinate of the principal point of the image, as a pixel offset from the left edge
            calib.left_imager_intrinsic.ppy         = table->intrinsic_left(1, 0);   // horizontal coordinate of the principal point of the image, as a pixel offset from the top edge
            calib.left_imager_intrinsic.coeffs[0]   = table->intrinsic_left(1, 1);   // Distortion coeeficients 1-5
            calib.left_imager_intrinsic.coeffs[1]   = table->intrinsic_left(1, 2);
            calib.left_imager_intrinsic.coeffs[2]   = table->intrinsic_left(2, 0);
            calib.left_imager_intrinsic.coeffs[3]   = table->intrinsic_left(2, 1);
            calib.left_imager_intrinsic.coeffs[4]   = table->intrinsic_left(2, 2);
            calib.left_imager_intrinsic.model       = rs_distortion::RS_DISTORTION_BROWN_CONRADY;

            calib.right_imager_intrinsic.width      = -1;    // applies for all resolutions
            calib.right_imager_intrinsic.height     = -1;
            calib.right_imager_intrinsic.fx         = table->intrinsic_right(0, 0);   // focal length of the image plane, as a multiple of pixel width
            calib.right_imager_intrinsic.fy         = table->intrinsic_right(0, 1);   // focal length of the image plane, as a multiple of pixel height
            calib.right_imager_intrinsic.ppx        = table->intrinsic_right(0, 2);   // horizontal coordinate of the principal point of the image, as a pixel offset from the left edge
            calib.right_imager_intrinsic.ppy        = table->intrinsic_right(1, 0);   // horizontal coordinate of the principal point of the image, as a pixel offset from the top edge
            calib.right_imager_intrinsic.coeffs[0]  = table->intrinsic_right(1, 1);   // Distortion coeeficients 1-5
            calib.right_imager_intrinsic.coeffs[1]  = table->intrinsic_right(1, 2);
            calib.right_imager_intrinsic.coeffs[2]  = table->intrinsic_right(2, 0);
            calib.right_imager_intrinsic.coeffs[3]  = table->intrinsic_right(2, 1);
            calib.right_imager_intrinsic.coeffs[4]  = table->intrinsic_right(2, 2);
            calib.right_imager_intrinsic.model      = rs_distortion::RS_DISTORTION_BROWN_CONRADY;

            // Fill in actual data. Note that only the Focal and Principal points data varies between different resolutions
            for (auto & i : { res_480_270, res_320_240, res_424_240, res_640_360, res_848_480, res_640_480, res_1280_720, res_1920_1080 })
            {
                calib.depth_intrinsic[i].width = resolutions_list[i].x;
                calib.depth_intrinsic[i].height = resolutions_list[i].y;

                calib.depth_intrinsic[i].fx = table->rect_params[i][0];
                calib.depth_intrinsic[i].fy = table->rect_params[i][1];
                calib.depth_intrinsic[i].ppx = table->rect_params[i][2];
                calib.depth_intrinsic[i].ppy = table->rect_params[i][3];
                calib.depth_intrinsic[i].model = rs_distortion::RS_DISTORTION_BROWN_CONRADY;
                memset(calib.depth_intrinsic[i].coeffs, 0, arr_size(calib.depth_intrinsic[i].coeffs));  // All coefficients are zeroed since rectified depth is defined as CS origin
            }
        }
        break;
        default:
            LOG_WARNING("Parsing Calibration table type " << table_id << " is not supported");
        }

        calib.data_present[table_id] = true;
    }

    void read_calibration(uvc::device & dev, std::timed_mutex & mutex, ds5_calibration& calib)
    {
        std::vector<uint8_t> table_raw_data;
        std::vector<calibration_table_id> actual_list = { coefficients_table_id };

        for (auto & id : actual_list)     // Fetch and parse calibration data
        {
            try
            {
                table_raw_data.clear();
                get_calibration_table_entry(dev, mutex, id, table_raw_data);

                parse_calibration_table( calib, id, table_raw_data);
            }
            catch (const std::runtime_error &e)
            {
                LOG_ERROR(e.what());
            }
            catch (...)
            {
                LOG_ERROR("Reading DS5 Calibration failed, table " << id);
            }
        }
    }

    void get_laser_power(const uvc::device & device, uint8_t & laser_power)
    {
        ds::xu_read(device, depth_xu, ds::control::ds5_lsr_power, &laser_power, sizeof(uint8_t));
    }

    void set_laser_power(uvc::device & device, uint8_t laser_power)
    {
        ds::xu_write(device, depth_xu, ds::control::ds5_lsr_power, &laser_power, sizeof(uint8_t));
    }

    void set_lr_exposure(uvc::device & device, uint16_t exposure)
    {
        ds::xu_write(device, depth_xu, ds::control::ds5_lr_exposure, exposure);
    }

    uint16_t get_lr_exposure(const uvc::device & device)
    {
        return ds::xu_read<uint16_t >(device, depth_xu, ds::control::ds5_lr_exposure);
    }
} // namespace rsimpl::ds5
} // namespace rsimpl
