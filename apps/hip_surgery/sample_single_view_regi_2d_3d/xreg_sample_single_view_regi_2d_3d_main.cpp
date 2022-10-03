/*
 * MIT License
 *
 * Copyright (c) 2020-2022 Robert Grupp
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <fmt/format.h>

#include <opencv2/imgcodecs.hpp>

#include "xregCSVUtils.h"
#include "xregEdgesFromRayCast.h"
#include "xregExceptionUtils.h"
#include "xregFilesystemUtils.h"
#include "xregHDF5.h"
#include "xregH5ProjDataIO.h"
#include "xregHUToLinAtt.h"
#include "xregITKBasicImageUtils.h"
#include "xregITKIOUtils.h"
#include "xregITKLabelUtils.h"
#include "xregITKOpenCVUtils.h"
#include "xregITKRemapUtils.h"
#include "xregMultivarNormDist.h"
#include "xregOpenCVUtils.h"
#include "xregProgOptUtils.h"
#include "xregRayCastProgOpts.h"
#include "xregRigidUtils.h"
#include "xregSampleUtils.h"
#include "xregSE3OptVars.h"
#include "xregStringUtils.h"

using namespace xreg;

struct SamplingToolData
{
  itk::Image<float,3>::Pointer ct_vol;
  itk::Image<unsigned char,3>::Pointer seg_vol;

  ProjDataF32 pd;

  FrameTransform gt_cam_extrins_to_pelvis_vol;
};

SamplingToolData ReadPelvisVolProjAndGtFromH5File(
  const std::string& h5_path,
  const std::string& spec_id_str,
  const size_type proj_idx,
  std::ostream& vout)
{
  SamplingToolData data;

  vout << "-----------------------------------------\n\n";
  vout << "reading data from HDF5 file..." << std::endl;

  vout << "opening source H5 for reading: " << h5_path << std::endl;
  H5::H5File h5(h5_path, H5F_ACC_RDONLY);

  if (!ObjectInGroupH5("proj-params", h5))
  {
    xregThrow("proj-params group not found in HDF5 file!");
  }

  vout << "setting up camera..." << std::endl;

  H5::Group proj_params_g = h5.openGroup("proj-params");

  data.pd.cam.setup(ReadMatrixH5CoordScalar("intrinsic", proj_params_g),
               ReadMatrixH5CoordScalar("extrinsic", proj_params_g),
               ReadSingleScalarH5ULong("num-rows", proj_params_g),
               ReadSingleScalarH5ULong("num-cols", proj_params_g),
               ReadSingleScalarH5CoordScalar("pixel-row-spacing", proj_params_g),
               ReadSingleScalarH5CoordScalar("pixel-col-spacing", proj_params_g));

  if (!ObjectInGroupH5(spec_id_str, h5))
  {
    xregThrow("specimen ID not found in HDF5 file: %s", spec_id_str.c_str());
  }

  H5::Group spec_g = h5.openGroup(spec_id_str);

  vout << "reading intensity volume..." << std::endl;
  data.ct_vol = ReadITKImageH5Float3D(spec_g.openGroup("vol"));

  vout << "reading segmentation volume..." << std::endl;
  const auto ct_labels = ReadITKImageH5UChar3D(spec_g.openGroup("vol-seg/image"));

  vout << "remapping all bones rigidly associated with pelvis to have label 1, "
          "and masking out the other labels (femurs)..." << std::endl;

  std::vector<unsigned char> lut(256, 0);
  lut[1] = 1;  // left hemi-pelvis
  lut[2] = 1;  // right hemi-pelvis
  lut[3] = 1;  // vertebra
  lut[4] = 1;  // upper sacrum
  lut[7] = 1;  // lower sacrum

  data.seg_vol = RemapITKLabelMap<unsigned char>(ct_labels.GetPointer(), lut);

  H5::Group projs_g = spec_g.openGroup("projections");

  const std::string proj_idx_str = fmt::format("{:03d}", proj_idx);

  if (!ObjectInGroupH5(proj_idx_str, projs_g))
  {
    xregThrow("projection not found: %s", proj_idx_str.c_str());
  }

  H5::Group proj_g = projs_g.openGroup(proj_idx_str);
        
  vout << "reading projection pixels..." << std::endl;

  data.pd.img = ReadITKImageH5Float2D(proj_g.openGroup("image"));

  vout << "setting rot-up field..." << std::endl;

  data.pd.rot_to_pat_up = ReadSingleScalarH5Bool("rot-180-for-up", proj_g) ?
                            ProjDataRotToPatUp::kONE_EIGHTY : ProjDataRotToPatUp::kZERO;

  data.gt_cam_extrins_to_pelvis_vol.matrix() = ReadMatrixH5CoordScalar("cam-to-pelvis-vol", proj_g.openGroup("gt-poses"));

  // Correct an inconsistency with linear interpolation texture indexing between when the ground truth
  // was constructed and now.
  {
    FrameTransform gt_corr = FrameTransform::Identity();
    gt_corr.matrix()(0,3) = -0.5f;
    gt_corr.matrix()(1,3) = -0.5f;
    gt_corr.matrix()(2,3) = -0.5f;

    data.gt_cam_extrins_to_pelvis_vol = gt_corr * data.gt_cam_extrins_to_pelvis_vol;
  }

  vout << "ground truth cam extrins to pelvis vol:\n" << data.gt_cam_extrins_to_pelvis_vol.matrix() << std::endl;

  vout << "-----------------------------------------\n\n";

  return data;
}

class PoseParamSampler
{
public:
  virtual MatMxN SamplePoseParams(const size_type num_samples, std::mt19937& rng_eng) = 0;
};

class PoseParamSamplerIndepNormalDims : public PoseParamSampler
{
public:
  PoseParamSamplerIndepNormalDims(
    const CoordScalar rot_x_deg_std_dev, const CoordScalar rot_y_deg_std_dev, const CoordScalar rot_z_deg_std_dev,
    const CoordScalar trans_x_mm_std_dev, const CoordScalar trans_y_mm_std_dev, const CoordScalar trans_z_mm_std_dev)
  {
    PtN std_devs(6);
    std_devs(0) = rot_x_deg_std_dev;
    std_devs(1) = rot_y_deg_std_dev;
    std_devs(2) = rot_z_deg_std_dev;
    std_devs(3) = trans_x_mm_std_dev;
    std_devs(4) = trans_y_mm_std_dev;
    std_devs(5) = trans_z_mm_std_dev;

    PtN mean(6);
    mean.setZero();

    dist_ = std::make_shared<MultivarNormalDistZeroCov>(mean, std_devs);
  }

  MatMxN SamplePoseParams(const size_type num_samples, std::mt19937& rng_eng) override
  {
    return dist_->draw_samples(num_samples, rng_eng);
  }

private:

  std::shared_ptr<MultivarNormalDistZeroCov> dist_;
};

int main(int argc, char* argv[])
{
  constexpr int kEXIT_VAL_SUCCESS        = 0;
  constexpr int kEXIT_VAL_BAD_USE        = 1;
  constexpr int kEXIT_VAL_BAD_INPUT_HDF5 = 2;

  // First, set up the program options

  ProgOpts po;

  xregPROG_OPTS_SET_COMPILE_DATE(po);

  po.set_help("TODO");
  
  po.set_arg_usage("<HDF5 Data File> <patient ID> <projection index> <num samples> <output directory>");
  po.set_min_num_pos_args(5);
  
  //po.add("batch-size", ProgOpts::kNO_SHORT_FLAG, ProgOpts::kSTORE_UINT32, "batch-size",
  //       "Maximum number of objective functions to evaluate at once on the GPU.")
  //  << ProgOpts::uint32(100);
  
  //po.add("ds-factor", 'd', ProgOpts::kSTORE_DOUBLE, "ds-factor",
  //       "Downsampling factor of each 2D projection dimension. 0.25 --> 4x downsampling in width AND height.")
  //  << 0.25;

  po.add("rng-seed", ProgOpts::kNO_SHORT_FLAG, ProgOpts::kSTORE_UINT32, "rng-seed",
         "A seed for the RNG engine. A random seed is drawn from random device when this is not provided.");

  po.add_backend_flags();

  try
  {
    po.parse(argc, argv);
  }
  catch (const ProgOpts::Exception& e)
  {
    std::cerr << "Error parsing command line arguments: " << e.what() << std::endl;
    po.print_usage(std::cerr);
    return kEXIT_VAL_BAD_USE;
  }

  if (po.help_set())
  {
    po.print_usage(std::cout);
    po.print_help(std::cout);
    return kEXIT_VAL_SUCCESS;
  }

  const bool verbose = po.get("verbose");
  std::ostream& vout = po.vout();
  
  const std::string ipcai_h5_data_path = po.pos_args()[0];
  const std::string spec_id_str        = po.pos_args()[1];
  const size_type   proj_idx           = StringCast<size_type>(po.pos_args()[2]);
  const size_type   num_samples        = StringCast<size_type>(po.pos_args()[3]);
  const std::string dst_dir_path       = po.pos_args()[4];

  const bool rng_seed_provided = po.has("rng-seed");

  //const size_type grid_batch_size = po.get("batch-size").as_uint32();

  //const double ds_factor = po.get("ds-factor");

  if (num_samples < 1)
  {
    std::cerr << "number of samples must be positive!" << std::endl;
    return kEXIT_VAL_BAD_USE;
  }

  const Path dst_dir = dst_dir_path;

  if (dst_dir.exists() && !dst_dir.is_dir())
  {
    std::cerr << "ERROR: output directory path exists, but is not a directory: " << dst_dir_path << std::endl;
    return kEXIT_VAL_BAD_USE;
  }

  if (!dst_dir.exists())
  {
    vout << "creating output directory..." << std::endl;
    MakeDirRecursive(dst_dir_path);
  }

  std::mt19937 rng_eng;

  if (rng_seed_provided)
  {
    const auto user_seed = po.get("rng-seed").as_uint32();

    vout << "using specified seed for RNG: " << user_seed << std::endl;

    rng_eng.seed(user_seed);
  }
  else
  {
    vout << "seeding RNG engine with random device..." << std::endl;
    SeedRNGEngWithRandDev(&rng_eng);
  }

  vout << "reading data from IPCAI HDF5 file..." << std::endl;

  const auto data_from_h5 = ReadPelvisVolProjAndGtFromH5File(
                              ipcai_h5_data_path, spec_id_str, proj_idx, vout);

  vout << "remapping proj to 8bpp for eventual edge overlay..." << std::endl;
  cv::Mat proj_remap = ShallowCopyItkToOpenCV(ITKImageRemap8bpp(
                          data_from_h5.pd.img.GetPointer()).GetPointer()).clone();

  vout << "masking out non-pelvis voxels and cropping..." << std::endl;
  auto ct_hu = MakeVolListFromVolAndLabels(data_from_h5.ct_vol.GetPointer(), data_from_h5.seg_vol.GetPointer(),
                                            { static_cast<unsigned char>(1) }, -1000.0f)[0];

  vout << "converting HU --> Lin. Att." << std::endl;
  auto ct_lin_att = HUToLinAtt(ct_hu.GetPointer());

  vout << "creating indep. normal dist. pose sampler..." << std::endl;
  auto param_sampler = std::make_shared<PoseParamSamplerIndepNormalDims>(
                            1.0 * kDEG2RAD, 1.0 * kDEG2RAD, 1.0 * kDEG2RAD, 1.0, 1.0, 5.0);

  MatMxN pose_param_samples(6, num_samples);
  
  // The first sample is always at ground truth
  vout << "setting first sample to zero..." << std::endl;
  pose_param_samples.col(0).setZero();

  vout << "sampling remaining " << (num_samples - 1) << " pose parameters..." << std::endl;
  pose_param_samples.block(0, 1, 6, num_samples - 1) = param_sampler->SamplePoseParams(num_samples - 1, rng_eng);

  //vout << "sampled pose params:\n" << pose_param_samples << std::endl;

  const Pt3 center_of_rot_wrt_vol = ITKVol3DCenterAsPhysPt(ct_lin_att.GetPointer());
  vout << "center of rot wrt vol:\n" << center_of_rot_wrt_vol << std::endl;

  const auto& cam = data_from_h5.pd.cam;

  const Pt3 center_of_rot_wrt_cam_proj_frame = cam.extrins * data_from_h5.gt_cam_extrins_to_pelvis_vol.inverse() * center_of_rot_wrt_vol;

  FrameTransform cam_proj_frame_shift_from_center_of_rot = FrameTransform::Identity();
  cam_proj_frame_shift_from_center_of_rot.matrix().block(0,3,3,1) = center_of_rot_wrt_cam_proj_frame;

  FrameTransform cam_proj_frame_shift_to_center_of_rot = FrameTransform::Identity();
  cam_proj_frame_shift_to_center_of_rot.matrix().block(0,3,3,1) = -center_of_rot_wrt_cam_proj_frame;

  const FrameTransform cam_extrins_to_center_of_rot_in_proj_frame =
                                  cam_proj_frame_shift_to_center_of_rot * cam.extrins;
  
  const FrameTransform center_of_rot_in_proj_frame_to_vol =
                                  data_from_h5.gt_cam_extrins_to_pelvis_vol *
                                  cam.extrins_inv *
                                  cam_proj_frame_shift_from_center_of_rot;

  vout << "converting parameters to 4x4 rigid transformation matrices..." << std::endl;

  SE3OptVarsLieAlg se3;

  FrameTransformList sampled_cam_to_pelvis_vol;
  sampled_cam_to_pelvis_vol.reserve(num_samples);

  CoordScalarList tmp_decomp_offset_row(8);
  std::vector<CoordScalarList> sampled_decomp_offsets;
  sampled_decomp_offsets.reserve(num_samples);

  CoordScalarList tmp_pose_csv_row(16);
  std::vector<CoordScalarList> sampled_poses_csv;
  sampled_poses_csv.reserve(num_samples);

  CoordScalarList tmp_pose_params_csv_row(6);
  std::vector<CoordScalarList> pose_params_csv;
  pose_params_csv.reserve(num_samples);

  EdgesFromRayCast edge_creator;
  edge_creator.do_canny = true;
  edge_creator.do_boundary = true;
  edge_creator.do_occ = false;
  edge_creator.cam = cam;
  edge_creator.vol = ct_hu;

  vout << "creating line integral ray caster object for edge images..." << std::endl;
  edge_creator.line_int_ray_caster = LineIntRayCasterFromProgOpts(po);

  vout << "creating depth ray caster object for edge_images..." << std::endl;
  edge_creator.boundary_ray_caster = DepthRayCasterFromProgOpts(po);

  for (size_type sample_idx = 0; sample_idx < num_samples; ++sample_idx)
  {
    vout << "processing sample index: " << sample_idx << std::endl;

    // convert pose parameters into a rigid transformation that is an offset from
    // ground truth with respect to the camera projective frame with a center of
    // rotation at the ground truth location of the volume centroid.
    const FrameTransform gt_off = se3(pose_param_samples.col(sample_idx));

    // Save the rigid offset in CSV format
    {
      int flat_idx = 0;
      for (int r = 0; r < 4; ++r)
      {
        for (int c = 0; c < 4; ++c, ++flat_idx)
        {
          tmp_pose_csv_row[flat_idx] = gt_off.matrix()(r,c);
        }
      }

      sampled_poses_csv.push_back(tmp_pose_csv_row);
    }

    // Save the se3 pose params
    for (int i = 0; i < 6; ++i)
    {
      tmp_pose_params_csv_row[i] = pose_param_samples(i, sample_idx);
    }
    pose_params_csv.push_back(tmp_pose_params_csv_row);

    // Save the rotation and translation magnitudes of the offset for later writing to CSV
    std::tie(tmp_decomp_offset_row[0], tmp_decomp_offset_row[1]) = ComputeRotAngTransMag(gt_off);
    tmp_decomp_offset_row[0] *= kRAD2DEG;

    // Save decompositions about the projective frame's axes for later writing to CSV
    std::tie(tmp_decomp_offset_row[2], tmp_decomp_offset_row[3],
             tmp_decomp_offset_row[4], tmp_decomp_offset_row[5],
             tmp_decomp_offset_row[6], tmp_decomp_offset_row[7]) = RigidXformToEulerXYZAndTrans(gt_off);
    tmp_decomp_offset_row[2] *= kRAD2DEG;
    tmp_decomp_offset_row[3] *= kRAD2DEG;
    tmp_decomp_offset_row[4] *= kRAD2DEG;

    sampled_decomp_offsets.push_back(tmp_decomp_offset_row);

    // Save the final composite (camera extrins to volume) transformation. These will be used later
    // for DRRs, 2D edges, and writing to CSV.
    const FrameTransform cam_extrins_to_vol =
      center_of_rot_in_proj_frame_to_vol *
      gt_off *
      cam_extrins_to_center_of_rot_in_proj_frame;

    sampled_cam_to_pelvis_vol.push_back(cam_extrins_to_vol);

    edge_creator.cam_wrt_vols = { cam_extrins_to_vol };

    vout << "  creating edges..." << std::endl;
    edge_creator();

    const std::string sample_idx_str = fmt::format("{:03d}", sample_idx);

    auto drr_img = edge_creator.line_int_ray_caster->proj(0);

    vout << "  saving raw DRR..." << std::endl;
    WriteITKImageToDisk(drr_img.GetPointer(), fmt::format("{}/drr_raw_{}.nii.gz", dst_dir_path, sample_idx_str));

    vout << "  remapping DRR..." << std::endl;
    cv::Mat drr_img_remap = ShallowCopyItkToOpenCV(ITKImageRemap8bpp(drr_img.GetPointer()).GetPointer()).clone();

    vout << "  saving DRR remap..." << std::endl;
    cv::imwrite(fmt::format("{}/drr_remap_{}.png", dst_dir_path, sample_idx_str), drr_img_remap);

    cv::Mat edges_ocv = ShallowCopyItkToOpenCV(edge_creator.final_edge_img.GetPointer());
    edges_ocv *= 255;  // useful for verifying output png has edges
    
    vout << "  saving edges..." << std::endl;
    cv::imwrite(fmt::format("{}/edges_{}.png", dst_dir_path, sample_idx_str), edges_ocv);

    vout << "  overlaying edges to projection..." << std::endl;
    cv::Mat edge_overlay_img = OverlayEdges(proj_remap, edges_ocv, 1);
    
    vout << "  saving edges overlay..." << std::endl;
    cv::imwrite(fmt::format("{}/edges_overlay_{}.png", dst_dir_path, sample_idx_str), edge_overlay_img);
  }

  vout << "writing offset CSV file..." << std::endl;
  WriteCSVFile(
    (dst_dir + "offset_amounts.csv").string(),
    sampled_decomp_offsets,
    {"total rotation (deg)", "total trans. (mm)",
     "rotation X (deg)", "rotation Y (deg)", "rotation Z (deg)",
     "translation X (mm)", "translation Y (mm)", "translation Z (mm)"});
  
  vout << "writing se(3) pose params CSV file..." << std::endl;
  WriteCSVFile(
    (dst_dir + "se3_lie_params.csv").string(),
    pose_params_csv,
    {"se3-dim-1", "se3-dim-2", "se3-dim-3", "se3-dim-4", "se3-dim-5", "se3-dim-6"});

  vout << "writing cam extrins. to vol pose CSV file..." << std::endl;
  WriteCSVFile(
    (dst_dir + "cam_extrins_to_vol_poses.csv").string(),
    sampled_poses_csv,
    {"row1_col1", "row1_col2", "row1_col3", "row1_col4",
     "row2_col1", "row2_col2", "row2_col3", "row2_col4",
     "row3_col1", "row3_col2", "row3_col3", "row3_col4",
     "row4_col1", "row4_col2", "row4_col3", "row4_col4"});

  vout << "exiting..." << std::endl;
  
  return kEXIT_VAL_SUCCESS;
}