/*
* Software License Agreement (BSD License)
*
*  Point Cloud Library (PCL) - www.pointclouds.org
*  Copyright (c) 2009-2012, Willow Garage, Inc.
*  Copyright (c) 2014, Stichting Centrum Wiskunde en Informatica.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of Willow Garage, Inc. nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*
*/
#ifndef POINT_CLOUD_CODECV2_IMPL_HPP
#define POINT_CLOUD_CODECV2_IMPL_HPP

#include <pcl/compression/entropy_range_coder.h>
#include <pcl/compression/impl/entropy_range_coder.hpp>

//
#include <pcl/cloud_codec_v2/point_cloud_codec_v2.h>
#include <pcl/compression/point_coding.h>
#include <pcl/compression/impl/octree_pointcloud_compression.hpp>


//includes to do the ICP procedures
#include <pcl/point_types.h>
#include <pcl/registration/icp.h>
#include <pcl/visualization/cloud_viewer.h>

// quality metrics, such that we can assess the quality of the ICP based transform
#include <pcl/quality/quality_metrics.h>
#include <pcl/quality/impl/quality_metrics_impl.hpp>

namespace pcl{

  namespace io{

    /// encoding routine, based on the PCL octree codec written by Julius Kammerl
    //////////////////////////////////////////////////////////////////////////////////////////////
    template<typename PointT, typename LeafT, typename BranchT, typename OctreeT> void OctreePointCloudCodecV2<
        PointT, LeafT, BranchT, OctreeT>::encodePointCloud (
        const PointCloudConstPtr &cloud_arg,
        std::ostream& compressed_tree_data_out_arg)
    {
      unsigned char recent_tree_depth =
          static_cast<unsigned char> (this->getTreeDepth ());

      // CWI addition to prevent crashes as in original cloud codec
      this->deleteCurrentBuffer();
	    this->deleteTree();

      // initialize octree
      this->setInputCloud (cloud_arg);

      // CWI added, when encoding hte output variable stores the (simplified icloud)
      this->output_ = PointCloudPtr(new PointCloud());

      // add point to octree
      this->addPointsFromInputCloud ();

      // make sure cloud contains points
      if (this->leaf_count_>0) {


        // color field analysis
        this->cloud_with_color_ = false;
        std::vector<pcl::PCLPointField> fields;
        int rgba_index = -1;
        rgba_index = pcl::getFieldIndex (*this->input_, "rgb", fields);
        if (rgba_index == -1)
        {
          rgba_index = pcl::getFieldIndex (*this->input_, "rgba", fields);
        }
        if (rgba_index >= 0)
        {
          this->point_color_offset_ = static_cast<unsigned char> (fields[rgba_index].offset);
          this->cloud_with_color_ = true;
        }

        // apply encoding configuration
		this->cloud_with_color_ &= this->do_color_encoding_;

        // if octree depth changed, we enforce I-frame encoding
        this->i_frame_ |= (recent_tree_depth != this->getTreeDepth ());// | !(iFrameCounter%10);

        // enable I-frame rate
        if (this->i_frame_counter_++==this->i_frame_rate_)
        {
          this->i_frame_counter_ =0;
          this->i_frame_ = true;
        }

        // increase frameID
        this->frame_ID_++;

        // do octree encoding
        if (!this->do_voxel_grid_enDecoding_)
        {
          this->point_count_data_vector_.clear ();
          this->point_count_data_vector_.reserve (cloud_arg->points.size ());
        }

        // initialize color encoding
        if(!color_coding_type_){
          this->color_coder_.initializeEncoding ();
          this->color_coder_.setPointCount (static_cast<unsigned int> (cloud_arg->points.size ()));
          this->color_coder_.setVoxelCount (static_cast<unsigned int> (this->leaf_count_));
        }else
        {
          this->jp_color_coder_.initializeEncoding ();
          this->jp_color_coder_.setPointCount (static_cast<unsigned int> (cloud_arg->points.size ()));
          this->jp_color_coder_.setVoxelCount (static_cast<unsigned int> (this->leaf_count_));
        }
        // initialize point encoding
        this->point_coder_.initializeEncoding ();
        this->centroid_coder_.initializeEncoding ();
        this->point_coder_.setPointCount (static_cast<unsigned int> (cloud_arg->points.size ()));
        this->centroid_coder_.initializeEncoding ();
        this->centroid_coder_.setPointCount( static_cast<unsigned int> (this->object_count_));

        // serialize octree
        if (this->i_frame_)
          // i-frame encoding - encode tree structure without referencing previous buffer
          this->serializeTree (this->binary_tree_data_vector_, false);
        else
          // p-frame encoding - XOR encoded tree structure
          this->serializeTree (this->binary_tree_data_vector_, true);


        // write frame header information to stream
        this->writeFrameHeader (compressed_tree_data_out_arg);

        // apply entropy coding to the content of all data vectors and send data to output stream
		this->entropyEncoding(compressed_tree_data_out_arg, compressed_tree_data_out_arg);

        // prepare for next frame
        this->switchBuffers ();
        this->i_frame_ = false;

        // reset object count
        this->object_count_ = 0;

        if (this->b_show_statistics_)
        {
          float bytes_per_XYZ = static_cast<float> (this->compressed_point_data_len_) / static_cast<float> (this->point_count_);
          float bytes_per_color = static_cast<float> (this->compressed_color_data_len_) / static_cast<float> (this->point_count_);

          PCL_INFO ("*** POINTCLOUD ENCODING ***\n");
          PCL_INFO ("Frame ID: %d\n", this->frame_ID_);
          if (this->i_frame_)
            PCL_INFO ("Encoding Frame: Intra frame\n");
          else
            PCL_INFO ("Encoding Frame: Prediction frame\n");
          PCL_INFO ("Number of encoded points: %ld\n", this->point_count_);
          PCL_INFO ("XYZ compression percentage: %f%%\n", bytes_per_XYZ / (3.0f * sizeof(float)) * 100.0f);
          PCL_INFO ("XYZ bytes per point: %f bytes\n", bytes_per_XYZ);
          PCL_INFO ("Color compression percentage: %f%%\n", bytes_per_color / (sizeof (int)) * 100.0f);
          PCL_INFO ("Color bytes per point: %f bytes\n", bytes_per_color);
          PCL_INFO ("Size of uncompressed point cloud: %f kBytes\n", static_cast<float> (this->point_count_) * (sizeof (int) + 3.0f  * sizeof (float)) / 1024);
          PCL_INFO ("Size of compressed point cloud: %d kBytes\n", (this->compressed_point_data_len_ + this->compressed_color_data_len_) / (1024));
          PCL_INFO ("Total bytes per point: %f\n", bytes_per_XYZ + bytes_per_color);
          PCL_INFO ("Total compression percentage: %f\n", (bytes_per_XYZ + bytes_per_color) / (sizeof (int) + 3.0f * sizeof(float)) * 100.0f);
          PCL_INFO ("Compression ratio: %f\n\n", static_cast<float> (sizeof (int) + 3.0f * sizeof (float)) / static_cast<float> (bytes_per_XYZ + bytes_per_color));
        }
      } else {
        if (this->b_show_statistics_)
        PCL_INFO ("Info: Dropping empty point cloud\n");
        this->deleteTree();
        this->i_frame_counter_ = 0;
        this->i_frame_ = true;
      }
    }

     /// decoding routine, based on the PCL octree codec written by Julius Kammerl
    //////////////////////////////////////////////////////////////////////////////////////////////
    template<typename PointT, typename LeafT, typename BranchT, typename OctreeT> void
    OctreePointCloudCodecV2<PointT, LeafT, BranchT, OctreeT>::decodePointCloud (
        std::istream& compressed_tree_data_in_arg,
        PointCloudPtr &cloud_arg)
    {

      // synchronize to frame header
      syncToHeader(compressed_tree_data_in_arg);

      // initialize octree
      this->switchBuffers ();
      
      // added to prevent crashes as happens with original cloud codec
	    this->deleteCurrentBuffer();
	    this->deleteTree();

      this->setOutputCloud (cloud_arg);

      // color field analysis
      this->cloud_with_color_ = false;
      std::vector<pcl::PCLPointField> fields;
      int rgba_index = -1;
      rgba_index = pcl::getFieldIndex (*this->output_, "rgb", fields);
      if (rgba_index == -1)
        rgba_index = pcl::getFieldIndex (*this->output_, "rgba", fields);
      if (rgba_index >= 0)
      {
        this->point_color_offset_ = static_cast<unsigned char> (fields[rgba_index].offset);
        this->cloud_with_color_ = true;
      }

      // read header from input stream
      this->readFrameHeader (compressed_tree_data_in_arg);

      // set the right grid pattern to the JPEG coder
      this->jp_color_coder_ = ColorCodingJPEG<PointT>(75,this->color_coding_type_);

      // decode data vectors from stream
	  this->entropyDecoding(compressed_tree_data_in_arg, compressed_tree_data_in_arg);

      // initialize color and point encoding
      if(!color_coding_type_)
        this->color_coder_.initializeDecoding ();
      else
        this->jp_color_coder_.initializeDecoding ();
      
      this->point_coder_.initializeDecoding ();
      this->centroid_coder_.initializeDecoding();

      // initialize output cloud
      this->output_->points.clear ();
      this->output_->points.reserve (static_cast<std::size_t> (this->point_count_));

      if (this->i_frame_)
        // i-frame decoding - decode tree structure without referencing previous buffer
        this->deserializeTree (this->binary_tree_data_vector_, false);
      else
        // p-frame decoding - decode XOR encoded tree structure
        this->deserializeTree (this->binary_tree_data_vector_, true);

      // assign point cloud properties
      this->output_->height = 1;
      this->output_->width = static_cast<uint32_t> (cloud_arg->points.size ());
      this->output_->is_dense = false;

      //! todo update fo cloud codecV2
      if (this->b_show_statistics_)
      {
        float bytes_per_XYZ = static_cast<float> (this->compressed_point_data_len_) / static_cast<float> (this->point_count_);
        float bytes_per_color = static_cast<float> (this->compressed_color_data_len_) / static_cast<float> (this->point_count_);

        PCL_INFO ("*** POINTCLOUDV2 DECODING ***\n");
        PCL_INFO ("Frame ID: %d\n", this->frame_ID_);
        if (this->i_frame_)
          PCL_INFO ("Encoding Frame: Intra frame\n");
        else
          PCL_INFO ("Encoding Frame: Prediction frame\n");
        PCL_INFO ("Number of encoded points: %ld\n", this->point_count_);
        PCL_INFO ("XYZ compression percentage: %f%%\n", bytes_per_XYZ / (3.0f * sizeof (float)) * 100.0f);
        PCL_INFO ("XYZ bytes per point: %f bytes\n", bytes_per_XYZ);
        PCL_INFO ("Color compression percentage: %f%%\n", bytes_per_color / (sizeof (int)) * 100.0f);
        PCL_INFO ("Color bytes per point: %f bytes\n", bytes_per_color);
        PCL_INFO ("Size of uncompressed point cloud: %f kBytes\n", static_cast<float> (this->point_count_) * (sizeof (int) + 3.0f * sizeof (float)) / 1024.0f);
        PCL_INFO ("Size of compressed point cloud: %f kBytes\n", static_cast<float> (this->compressed_point_data_len_ + this->compressed_color_data_len_) / 1024.0f);
        PCL_INFO ("Total bytes per point: %d bytes\n", static_cast<int> (bytes_per_XYZ + bytes_per_color));
        PCL_INFO ("Total compression percentage: %f%%\n", (bytes_per_XYZ + bytes_per_color) / (sizeof (int) + 3.0f * sizeof (float)) * 100.0f);
        PCL_INFO ("Compression ratio: %f\n\n", static_cast<float> (sizeof (int) + 3.0f * sizeof (float)) / static_cast<float> (bytes_per_XYZ + bytes_per_color));
      }
    }

    /*!
    \brief  helper function to compute the delta frames
    \author Rufael Mekuria rufael.mekuria@cwi.nl
    \param  const PointCloudConstPtr &pcloud_arg_in  input argument, cloud to simplify
    \param  OctreePointCloudCompression<PointT,LeafT,BranchT,OctreeT> *octree_simplifier, resulting octree datastructure that can be used for compression of the octree
    \param  out_cloud  output simplified point cloud
    */
    template<typename PointT, typename LeafT, typename BranchT, typename OctreeT> void
      OctreePointCloudCodecV2<PointT,LeafT,BranchT,OctreeT>::simplifyPCloud(const PointCloudConstPtr &pcloud_arg_in, 
      OctreePointCloudCompression<PointT,LeafT,BranchT,OctreeT> *octree_simplifier,
      PointCloudPtr &out_cloud )
    {
      // this is the octree coding part of the predictive encoding
      octree_simplifier = new OctreePointCloudCompression<PointT,LeafT,BranchT,OctreeT>
      (
        MANUAL_CONFIGURATION,
        false,
        this->point_resolution_,
        this->octree_resolution_,
        true,
        0,
        true,
        this->color_bit_resolution_ /*,0,do_voxel_centroid_enDecoding_*/
      );

      // use bounding box and obtain the octree grid
      octree_simplifier->defineBoundingBox(0.0, 0.0, 0.0, 1.0, 1.0, 1.0);
      octree_simplifier->setInputCloud (pcloud_arg_in);
      octree_simplifier->addPointsFromInputCloud ();

      /////////////////////////////////////////////////////////////////////////
      //! initialize output cloud
      //PointCloudPtr out_cloud(new PointCloud( octree_simplifier.leaf_count_, 1));
	  out_cloud->width = (uint32_t) octree_simplifier->getLeafCount();
      out_cloud->height = 1;
      out_cloud->points.reserve(octree_simplifier->getLeafCount());  
      // cant get access to the number of leafs
      ////////////////////////////////////////////////////////////////////////////

      ///////////// compute the simplified cloud by iterating the octree ////////
      octree::OctreeLeafNodeIterator<OctreeT> it_ = octree_simplifier->leaf_begin();
      octree::OctreeLeafNodeIterator<OctreeT> it_end = octree_simplifier->leaf_end();

      for(int l_index =0;it_ !=it_end; it_++, l_index++)
      {
        // new point for the simplified cloud
        PointT l_new_point;
        
        //! centroid for storage
        std::vector<int>& point_indices = it_.getLeafContainer().getPointIndicesVector();
        
        // if centroid coding, store centroid, otherwise add 
        if(!do_voxel_centroid_enDecoding_)
        {
          octree_simplifier->genLeafNodeCenterFromOctreeKey(it_.getCurrentOctreeKey(),l_new_point);
        }
        else
        {
          Eigen::Vector4f cent;
          pcl::compute3DCentroid<PointT>(*pcloud_arg_in, point_indices, cent);

          l_new_point.x = cent[0];
          l_new_point.y = cent[1];
          l_new_point.z = cent[2];
        }
        long color_r=0;
        long color_g=0;
        long color_b=0;

        //! compute average color
        for(int i=0; i< point_indices.size();i++)
        {
          color_r+=pcloud_arg_in->points[point_indices[i]].r;
          color_g+=pcloud_arg_in->points[point_indices[i]].g;
          color_b+=pcloud_arg_in->points[point_indices[i]].b;
        }

        l_new_point.r = (char) (color_r / point_indices.size());
        l_new_point.g = (char) (color_g / point_indices.size());
        l_new_point.b = (char) (color_b / point_indices.size());
     
        out_cloud->points.push_back(l_new_point);
      }
      //////////////// done computing simplified cloud and octree structure //////////////////
    };

    /*!
    \brief function to compute the delta frame, can be used to implement I and p frames coding later on
    \author Rufael Mekuria rufael.mekuria@cwi.nl
    */
    template<typename PointT, typename LeafT, typename BranchT, typename OctreeT> void 
      OctreePointCloudCodecV2<PointT,LeafT,BranchT,OctreeT>::generatePointCloudDeltaFrame( 
      const PointCloudConstPtr &icloud_arg /* icloud is already stored but can also be given as argument */,
      const PointCloudConstPtr &pcloud_arg,
      PointCloudPtr &out_cloud_arg, /* write the output cloud so we can assess the quality resulting from this algorithm */
      std::ostream& i_coded_data, 
      std::ostream& p_coded_data, 
      bool icp_on_original,
      bool write_out_cloud)
    {
      ///////////////////////////// IFRAME MACROBLOCKS I_M ////////////////////////////
      // set boundingbox to normalized cube and compute macroblocks
      // compute 16x16x16 octree 
      // store the keys to macroblock map
      OctreePointCloudCompression<PointT,LeafT,BranchT,OctreeT> i_coder
      (
        MANUAL_CONFIGURATION,
        false,
        this->point_resolution_,
        this->octree_resolution_ * macroblock_size,
        true,
        0,
        true,
        this->color_bit_resolution_ 
        /*,0,do_voxel_centroid_enDecoding_*/
      );

      // I frame coder
      i_coder.defineBoundingBox(0.0, 0.0, 0.0, 1.0, 1.0, 1.0);
      i_coder.setInputCloud (icloud_arg); /* assume right argument was given, either the original p cloud or an i cloud */
      i_coder.addPointsFromInputCloud ();

      std::map<pcl::octree::OctreeKey,std::vector<int> *,std::less<pcl::octree::OctreeKey> > I_M;
      
      octree::OctreeLeafNodeIterator<OctreeT> it_i_frame = i_coder.leaf_begin();
      octree::OctreeLeafNodeIterator<OctreeT> it_i_end = i_coder.leaf_end();
    
      for(;it_i_frame!=it_i_end;++it_i_frame)
      {		
        std::vector<int> &leaf_data = it_i_frame.getLeafContainer().getPointIndicesVector() ;
        I_M[it_i_frame.getCurrentOctreeKey()] = &leaf_data;
      }
      ///////////////////////////// ~IFRAME MACROBLOCKS I_M ////////////////////////////


      ///////////////////////////// PFRAME MACROBLOCKS P_M /////////////////////////////
  
      ////////// 1. fine grained octree and simplified cloud for P frame /////////////////
      PointCloudPtr simp_pcloud(new PointCloud());
      OctreePointCloudCompression<PointT,LeafT,BranchT,OctreeT> *octree_pcloud = NULL;

      if(!icp_on_original){
        simplifyPCloud(pcloud_arg,octree_pcloud, simp_pcloud);
      }
      ////////// 2. Compute Macroblocks fo P Frame //////////////////////////////////////////////////
      // the macroblocks of the p_frame, this is done on top of the more finegrained intraframe octree of P
      OctreePointCloudCompression<PointT,LeafT,BranchT,OctreeT> p_coder
        (
        MANUAL_CONFIGURATION,
        false,
        this->point_resolution_,
        this->octree_resolution_ * macroblock_size,
        true,
        0,
        true,
        this->color_bit_resolution_
        );

      p_coder.defineBoundingBox(0.0, 0.0, 0.0, 1.0, 1.0, 1.0);
      
      // either do the icp on the original or simplified clouds
      p_coder.setInputCloud(icp_on_original ? pcloud_arg:simp_pcloud);
      p_coder.addPointsFromInputCloud ();

      //! macroblocks that or non empty in both the I Frame and the P Frame
      std::map<pcl::octree::OctreeKey,std::pair<std::vector<int> *
        ,std::vector<int> *>,std::less<pcl::octree::OctreeKey> > S_M;

      //! macroblocks only in P_M
      std::map<pcl::octree::OctreeKey,std::vector<int> *,
        std::less<pcl::octree::OctreeKey> > P_M;

      ////////////////// iterate the predictive frame and find common macro blocks /////////////
      octree::OctreeLeafNodeIterator<OctreeT> it_predictive = p_coder.leaf_begin();
      octree::OctreeLeafNodeIterator<OctreeT> it_predictive_end = p_coder.leaf_end();
      
      // initialize the ouput cloud
      out_cloud_arg->height=1;
      out_cloud_arg->width =0;

      // point cloud for storing all intra encoded points
      typename pcl::PointCloud<PointT>::Ptr intra_coded_points(new pcl::PointCloud<PointT>());

      for(;it_predictive!=it_predictive_end;++it_predictive)
      {
        if(I_M.count(it_predictive.getCurrentOctreeKey()))
        {
          // macroblocks in both frames, we will use ICP to do inter predictive coding
          S_M[it_predictive.getCurrentOctreeKey()]=make_pair(I_M[it_predictive.getCurrentOctreeKey()],
                                                             &(it_predictive.getLeafContainer().getPointIndicesVector())) ;
        }
        else
        {
          P_M[it_predictive.getCurrentOctreeKey()] = &(it_predictive.getLeafContainer().getPointIndicesVector());
          // out_cloud_arg-> exclusively coded frame
          std::vector<int> &l_pts = * P_M[it_predictive.getCurrentOctreeKey()];
          for(int i=0; i < l_pts.size();i++ ){
            out_cloud_arg->push_back( icp_on_original ? (*pcloud_arg)[l_pts[i]]: (*simp_pcloud)[l_pts[i]]);
          }
        }
      }

      cout << " done creating macroblocks, shared macroblocks: " 
        << S_M.size() << " exclusive macroblocks: " << P_M.size() << std::endl; 
      shared_macroblock_percentage = (float) (S_M.size()) / ((float) P_M.size() + S_M.size() );
      /////////////////////////////////////////////////////////////////////////////////////////

      //////////////// ITERATE THE SHARED MACROBLOCKS AND DO ICP BASED PREDICTION //////////////
      std::map<pcl::octree::OctreeKey,std::pair<std::vector<int> *
        ,std::vector<int> *>,std::less<pcl::octree::OctreeKey> >::iterator sm_it;

      cout << " iterating shared macroblocks, block count = " << S_M.size() <<std::endl;
      int conv_count=0;

      // iterate the common macroblocks and do ICP
      for(sm_it = S_M.begin(); sm_it != S_M.end(); ++sm_it)
      { 
        /* hard copy of the point clouds */
        typename pcl::PointCloud<PointT>::Ptr cloud_in (new pcl::PointCloud<PointT>(*icloud_arg,  *(sm_it->second.first)));
        typename pcl::PointCloud<PointT>::Ptr cloud_out (new pcl::PointCloud<PointT>(icp_on_original ? *pcloud_arg : *simp_pcloud, *(sm_it->second.second)));

        // check on the point count
        bool do_icp =  cloud_out->size() > 6 ? (cloud_out->size() < cloud_in->size() * 2) &&  (cloud_out->size() >= cloud_in->size() * 0.5)  : false;

        double in_av[3]={0,0,0};
        double out_av[3]={0,0,0};
        double in_var=0;
        double out_var=0;

        // compute the average colors
        if(do_icp)
        {
          // create references to ease acccess via [] operator in the cloud
          pcl::PointCloud<PointT> & rcloud_out = *cloud_out;
          pcl::PointCloud<PointT> & rcloud_in = *cloud_in;

          for(int i=0; i<cloud_in->size();i++)
          {
            in_av[0]+= (double) rcloud_in[i].r;
            in_av[1]+= (double) rcloud_in[i].g;
            in_av[2]+= (double) rcloud_in[i].b;
          }

          in_av[0]/=rcloud_in.size();
          in_av[1]/=rcloud_in.size();
          in_av[2]/=rcloud_in.size();
          
          // variance
          for(int i=0; i<rcloud_in.size();i++)
          {
            double val= (rcloud_in[i].r - in_av[0]) * (rcloud_in[i].r - in_av[0]) + 
              (rcloud_in[i].g - in_av[1]) * (rcloud_in[i].g - in_av[1]) +
              (rcloud_in[i].b - in_av[2]) * (rcloud_in[i].b - in_av[2]);

            in_var+=val;
          }
          in_var/=(3*rcloud_in.size());

          //
          for(int i=0; i<rcloud_out.size();i++)
          {
            out_av[0]+= (double) rcloud_out[i].r;
            out_av[1]+= (double) rcloud_out[i].g;
            out_av[2]+= (double) rcloud_out[i].b;
          }
          out_av[0]/=rcloud_out.size();
          out_av[1]/=rcloud_out.size();
          out_av[2]/=rcloud_out.size();

          for(int i=0; i<rcloud_out.size();i++)
          {
            double val= (rcloud_out[i].r - out_av[0]) * (rcloud_out[i].r - out_av[0]) + 
              (rcloud_out[i].g - out_av[1]) * (rcloud_out[i].g - out_av[1]) +
              (rcloud_out[i].b - out_av[2]) * (rcloud_out[i].b - out_av[2]);

            out_var+=val;
          }
          out_var/=(3*rcloud_out.size());

          //cout << "input mean rgb = " << in_av[0] << "  , " << in_av[1] << "  , " << in_av[2] << endl;
          //cout << "output mean rgb = " << out_av[0] << "  , " << out_av[1] << "  , " << out_av[2] << endl;
          //cout << " input variance = " << in_var << " number of inputs " << rcloud_in.size() << endl;
          //cout << " output variance = " << out_var << " number of outputs " << rcloud_out.size() << endl;
          //cin.get();

          // for segments with large variance, skip the prediction
          if(in_var > 100 || out_var > 100)
            do_icp = false;
        }

        // compute color offsets
        char rgb_offset_r=0; 
        char rgb_offset_g=0; 
        char rgb_offset_b=0; 

        if(std::abs(out_av[0] - in_av[0]) < 32)
          rgb_offset_r = (char)(out_av[0] - in_av[0]);
        if(std::abs(out_av[1] - in_av[1]) < 32)
          rgb_offset_g = (char)(out_av[1] - in_av[1]);
        if(std::abs(out_av[2] - in_av[2]) < 32)
          rgb_offset_b = (char)(out_av[2] - in_av[2]);

        if(do_icp){

          pcl::IterativeClosestPoint<PointT, PointT> icp;

          icp.setInputCloud(cloud_in);
          icp.setInputTarget(cloud_out);

          icp.setMaximumIterations (50);
          // Set the transformation epsilon (criterion 2)
          icp.setTransformationEpsilon (1e-8);
          // Set the euclidean distance difference epsilon (criterion 3)
          icp.setEuclideanFitnessEpsilon (this->point_resolution_);

          pcl::PointCloud<PointT> Final;
          icp.align(Final);

          // compute the fitness for the colors

          if(icp.hasConverged() && icp.getFitnessScore() < this->point_resolution_ * 4 )
          {

            //cout << "has converged:" << icp.hasConverged() << " score: " <<
            //icp.getFitnessScore() << std::endl;
            //cout << icp.getFinalTransformation() << std::endl;
            //std::cin.get();
            // out_cloud_arg->
            conv_count++;

            // copy predicted points as the ICP prediction has converged
            for(int i=0; i < Final.size();i++ )
            {
              // compensate color offsets, ( remove if this does not give a better result)
              if(Final[i].r > 32 && Final[i].r < 223 )
                Final[i].r+= rgb_offset_r;
              if(Final[i].g > 32 && Final[i].g < 223 )
                Final[i].g+= rgb_offset_g;
              if(Final[i].b > 32 && Final[i].b < 223 )
                Final[i].b+= rgb_offset_b;

              // push back the points
              out_cloud_arg->push_back(Final[i]);
            }

           // for now estimate 16 bytes for each voxel
            char p_dat[16]={};
            p_coded_data.write(p_dat,16);

            //cout << " predicted " << Final.size() << " points, from " << cloud_out->size() << " points " << endl;
            //cin.get();
          }
          else
          {
            // copy original points as the icp prediction has not converged
            for(int i=0; i < cloud_out->size();i++ ){
              out_cloud_arg->push_back((*cloud_out)[i]);
              intra_coded_points->push_back((*cloud_out)[i]);
            }
          }
        }
        else{
          // copy original points as the icp prediction has not converged
          for(int i=0; i < cloud_out->size();i++ ){
            out_cloud_arg->push_back((*cloud_out)[i]);
            intra_coded_points->push_back((*cloud_out)[i]);
          }
        }
      }

      // add the intra coded points
#if __cplusplus >= 201103L
      for(auto pm_it = P_M.begin(); pm_it != P_M.end(); ++pm_it)
#else
      for(std::map<pcl::octree::OctreeKey,std::vector<int> *>::iterator pm_it = P_M.begin(); pm_it != P_M.end(); ++pm_it)
#endif//__cplusplus >= 201103L
        {
        /* hard copy of the point clouds */
        typename pcl::PointCloud<PointT>::Ptr cloud_out (new pcl::PointCloud<PointT>(icp_on_original ? *pcloud_arg : *simp_pcloud, *(pm_it->second)));
        for(int i=0; i < cloud_out->size();i++){
          out_cloud_arg->push_back((*cloud_out)[i]);
          intra_coded_points->push_back((*cloud_out)[i]);
        }
      }
      std::cout << " done, convergence percentage: " << ((float) conv_count) / S_M.size()   << std::endl;
      shared_macroblock_convergence_percentage = ((float) conv_count) / (float) S_M.size() ; // convergence percentage
      //////////////////////////////////////////////////////////////////////////////////////////////

      OctreePointCloudCompression<PointT,LeafT,BranchT,OctreeT> intra_coder
      (
        MANUAL_CONFIGURATION,
        false,
        this->point_resolution_,
        this->octree_resolution_,
        true,
        0,
        true,
        this->color_bit_resolution_
        /*,0,do_voxel_centroid_enDecoding_*/
      );

      // encode the intrapoints
      intra_coder.encodePointCloud(intra_coded_points, i_coded_data);
    }

  /*
    if(icp.hasConverged() ){
      quality::QualityMetric transformed_q;
      quality::QualityMetric transformed_b;

      quality::computeQualityMetric<PointT>( *cloud_out, Final, transformed_q);
      std::cout << " results with transform " << std::endl;
      std::cout << " psnr dB: " << transformed_q.psnr_db << std::endl;
      std::cout << " mse error rms: " << transformed_q.symm_rms << std::endl;
      std::cout << " mse error hauss: " << transformed_q.symm_hausdorff << std::endl;
      std::cout << " psnr Color: " <<  transformed_q.psnr_yuv[0]<< std::endl;

      quality::computeQualityMetric<PointT>( *cloud_out , *cloud_in, transformed_b);
      std::cout << " results without transform " << std::endl;
      std::cout << " psnr dB: " << transformed_b.psnr_db << std::endl;
      std::cout << " mse error rms: " << transformed_b.symm_rms << std::endl;
      std::cout << " mse error hauss: " << transformed_b.symm_hausdorff << std::endl;
      std::cout << " psnr Color: " <<  transformed_b.psnr_yuv[0]<< std::endl;
    }*/

    /////////////////////////////////////////////////////////////////////////////////////////////////
    template<typename PointT, typename LeafT, typename BranchT, typename OctreeT> void
      OctreePointCloudCodecV2<PointT, LeafT, BranchT, OctreeT>::decodePointCloudDeltaFrame( 
          const PointCloudConstPtr &icloud_arg, const PointCloudConstPtr &pcloud_arg, 
          std::istream& i_coded_data, std::istream& p_coded_data)
    {}

    /////////////////// CODE OVERWRITING CODEC V1 to become codec V2 ////////////////////////////////

    //! write frame header, extended for cloud codecV2
    template<typename PointT, typename LeafT, typename BranchT, typename OctreeT> void
      OctreePointCloudCodecV2<PointT, LeafT, BranchT, OctreeT>::writeFrameHeader(std::ostream& compressed_tree_data_out_arg)
    {
      compressed_tree_data_out_arg.write (reinterpret_cast<const char*> (frame_header_identifier_), strlen (frame_header_identifier_));
      //! use the base class and write some extended information on codecV2
      OctreePointCloudCompression<PointT, LeafT, BranchT, OctreeT>::writeFrameHeader(compressed_tree_data_out_arg);

      //! write additional fields for cloud codec v2
      compressed_tree_data_out_arg.write (reinterpret_cast<const char*> (&do_voxel_centroid_enDecoding_), sizeof (do_voxel_centroid_enDecoding_));
      compressed_tree_data_out_arg.write (reinterpret_cast<const char*> (&do_connectivity_encoding_), sizeof (do_connectivity_encoding_));
      compressed_tree_data_out_arg.write (reinterpret_cast<const char*> (&create_scalable_bitstream_), sizeof (create_scalable_bitstream_));
      compressed_tree_data_out_arg.write (reinterpret_cast<const char*> (&color_coding_type_), sizeof (color_coding_type_));
    };

    //! read Frame header, extended for cloud codecV2
    template<typename PointT, typename LeafT, typename BranchT, typename OctreeT> void
      OctreePointCloudCodecV2<PointT, LeafT, BranchT, OctreeT>::readFrameHeader(std::istream& compressed_tree_data_in_arg)
    {
      //! use the base class and read some extended information on codecV2
      OctreePointCloudCompression<PointT>::readFrameHeader(compressed_tree_data_in_arg);

      //! read additional fields for cloud codec v2
      compressed_tree_data_in_arg.read (reinterpret_cast<char*> (&do_voxel_centroid_enDecoding_), sizeof (do_voxel_centroid_enDecoding_));
      compressed_tree_data_in_arg.read (reinterpret_cast<char*> (&do_connectivity_encoding_), sizeof (do_connectivity_encoding_));
      compressed_tree_data_in_arg.read (reinterpret_cast<char*> (&create_scalable_bitstream_), sizeof (create_scalable_bitstream_));
      compressed_tree_data_in_arg.read (reinterpret_cast<char*> (&color_coding_type_), sizeof (color_coding_type_));
    };

    /** \brief Encode leaf node information during serialization
    * \param leaf_arg: reference to new leaf node
    * \param key_arg: octree key of new leaf node
    * \brief added centroids encoding compared to the original codec
    */
    template<typename PointT, typename LeafT, typename BranchT, typename OctreeT> void
      OctreePointCloudCodecV2<PointT, LeafT, BranchT, OctreeT>::serializeTreeCallback (LeafT &leaf_arg, const OctreeKey& key_arg)
    {
      // reference to point indices vector stored within octree leaf //
      const std::vector<int>& leafIdx = leaf_arg.getPointIndicesVector();
      double lowerVoxelCorner[3];
      PointT centroid;
      // //

      // calculate lower voxel corner based on octree key //
      lowerVoxelCorner[0] = static_cast<double> (key_arg.x) * this->resolution_ + this->min_x_;
      lowerVoxelCorner[1] = static_cast<double> (key_arg.y) * this->resolution_ + this->min_y_;
      lowerVoxelCorner[2] = static_cast<double> (key_arg.z) * this->resolution_ + this->min_z_;
      // //

      // points detail encoding
      if (!this->do_voxel_grid_enDecoding_)
      {
        // encode amount of points within voxel
        this->point_count_data_vector_.push_back (static_cast<int> (leafIdx.size ()));

        // differentially encode points to lower voxel corner
        this->point_coder_.encodePoints (leafIdx, lowerVoxelCorner, this->input_);

        if (this->cloud_with_color_) {
          // encode color of points
          if(!this->color_coding_type_) {
            this->color_coder_.encodePoints (leafIdx, this->point_color_offset_, this->input_);
          } else {
            jp_color_coder_.encodePoints (leafIdx, this->point_color_offset_, this->input_);
          }
        }
      }
      else // centroid or voxelgrid encoding
      {
        if (this->cloud_with_color_)
        {
          // encode average color of all points within voxel
          if(!this->color_coding_type_)
            this->color_coder_.encodeAverageOfPoints (leafIdx, this->point_color_offset_, this->input_);
          else
            this->jp_color_coder_.encodeAverageOfPoints (leafIdx, this->point_color_offset_, this->input_);
          std::vector<char> & l_colors = this->color_coding_type_ ?this->jp_color_coder_.getAverageDataVectorB() : this->color_coder_.getAverageDataVector();

          //! get the colors from the vector from the color coder, use to store to do temporal prediction
          centroid.r =  l_colors[l_colors.size() - 1];
          centroid.g =  l_colors[l_colors.size() - 2];
          centroid.b =  l_colors[l_colors.size() - 3];
        }
        if(!do_voxel_centroid_enDecoding_)
        {
          centroid.x = lowerVoxelCorner[0] + 0.5 * this->resolution_ ;
          centroid.y = lowerVoxelCorner[1] + 0.5 * this->resolution_ ;
          centroid.z = lowerVoxelCorner[2] + 0.5 * this->resolution_ ;
        }
        else
        {
          Eigen::Vector4f f_centroid;
          pcl::compute3DCentroid<PointT>(*(this->input_), leafIdx, f_centroid);

          centroid.x = f_centroid[0];
          centroid.y = f_centroid[1];
          centroid.z = f_centroid[2];

          this->centroid_coder_.encodePoint(lowerVoxelCorner, centroid);
        }
        // store the simplified cloud so that it can be used for predictive encoding
        this->output_->points.push_back(centroid);
      }
    }

    /** \brief Decode leaf nodes information during deserialization
    * \param key_arg octree key of new leaf node
    * \brief added centroids encoding compared to the original codec
    */
    // param leaf_arg reference to new leaf node
    template<typename PointT, typename LeafT, typename BranchT, typename OctreeT> void
      OctreePointCloudCodecV2<PointT, LeafT, BranchT, OctreeT>::deserializeTreeCallback (LeafT&, const OctreeKey& key_arg)
    {
      double lowerVoxelCorner[3];
      std::size_t pointCount, i, cloudSize;
      PointT newPoint;
      pointCount = 1;

      if (!this->do_voxel_grid_enDecoding_)
      {
        // get current cloud size
		cloudSize = this->output_->points.size();

        // get amount of point to be decoded
        pointCount = *this->point_count_data_vector_iterator_;
        this->point_count_data_vector_iterator_++;

        // increase point cloud by amount of voxel points
        for (i = 0; i < pointCount; i++)
          this->output_->points.push_back (newPoint);

        // calculcate position of lower voxel corner
        lowerVoxelCorner[0] = static_cast<double> (key_arg.x) * this->resolution_ + this->min_x_;
        lowerVoxelCorner[1] = static_cast<double> (key_arg.y) * this->resolution_ + this->min_y_;
        lowerVoxelCorner[2] = static_cast<double> (key_arg.z) * this->resolution_ + this->min_z_;

        // decode differentially encoded points
        this->point_coder_.decodePoints (this->output_, lowerVoxelCorner, cloudSize, cloudSize + pointCount);
      }
      else
      {
        // decode the centroid or the voxel center
        if(do_voxel_centroid_enDecoding_)
        {
          PointT centroid_point;

          // calculcate position of lower voxel corner
          lowerVoxelCorner[0] = static_cast<double> (key_arg.x) * this->resolution_ + this->min_x_;
          lowerVoxelCorner[1] = static_cast<double> (key_arg.y) * this->resolution_ + this->min_y_;
          lowerVoxelCorner[2] = static_cast<double> (key_arg.z) * this->resolution_ + this->min_z_;

          centroid_coder_.decodePoint(newPoint,lowerVoxelCorner);
        }
        else
        {
          // calculate center of lower voxel corner
          newPoint.x = static_cast<float> ((static_cast<double> (key_arg.x) + 0.5) * this->resolution_ + this->min_x_);
          newPoint.y = static_cast<float> ((static_cast<double> (key_arg.y) + 0.5) * this->resolution_ + this->min_y_);
          newPoint.z = static_cast<float> ((static_cast<double> (key_arg.z) + 0.5) * this->resolution_ + this->min_z_);
        }

        // add point to point cloud
        this->output_->points.push_back (newPoint);
      }

      if (this->cloud_with_color_)
      {
        if (this->data_with_color_)
          // decode color information
          if(!this->color_coding_type_)
            this->color_coder_.decodePoints (this->output_, this->output_->points.size () - pointCount,
					     this->output_->points.size (), this->point_color_offset_);
          else
            this->jp_color_coder_.decodePoints (this->output_, this->output_->points.size () - pointCount,
						this->output_->points.size (), this->point_color_offset_);
        else
          // set default color information
          
		  this->color_coder_.setDefaultColor(this->output_, this->output_->points.size() - pointCount,
		  this->output_->points.size(), this->point_color_offset_);
      }
    }

    /** \brief Synchronize to frame header
    * \param compressed_tree_data_in_arg: binary input stream
    * \brief use the new v2 header
    */
    template<typename PointT, typename LeafT, typename BranchT, typename OctreeT> void
      OctreePointCloudCodecV2<PointT, LeafT, BranchT, OctreeT>::syncToHeader (std::istream& compressed_tree_data_in_arg)
    {
      // sync to frame header
      unsigned int header_id_pos = 0;
      while (header_id_pos < strlen (this->frame_header_identifier_))
      {
        char readChar;
        compressed_tree_data_in_arg.read (static_cast<char*> (&readChar), sizeof (readChar));
        if (readChar != this->frame_header_identifier_[header_id_pos++])
          header_id_pos = (this->frame_header_identifier_[0]==readChar)?1:0;
      }
      //! read the original octree header
      OctreePointCloudCompression<PointT>::syncToHeader (compressed_tree_data_in_arg);
    };

    /** \brief Apply entropy encoding to encoded information and output to binary stream, added bitstream scalability and centroid encoding compared to  V1
    * \param compressed_tree_data_out_arg: binary output stream
    */
    template<typename PointT, typename LeafT, typename BranchT, typename OctreeT> void
      OctreePointCloudCodecV2<PointT, LeafT, BranchT, OctreeT>::entropyEncoding (std::ostream& compressed_tree_data_out_arg1, std::ostream& compressed_tree_data_out_arg2 )
    {
      uint64_t binary_tree_data_vector_size;
      uint64_t point_avg_color_data_vector_size;

      this->compressed_point_data_len_ = 0;
      this->compressed_color_data_len_ = 0;

      // encode binary octree structure
      binary_tree_data_vector_size = this->binary_tree_data_vector_.size ();
      compressed_tree_data_out_arg1.write (reinterpret_cast<const char*> (&binary_tree_data_vector_size), sizeof (binary_tree_data_vector_size));
	  this->compressed_point_data_len_ += this->entropy_coder_.encodeCharVectorToStream(this->binary_tree_data_vector_, compressed_tree_data_out_arg1);

      // store the number of bytes used for voxel encoding
      compression_performance_metrics[0] = this->compressed_point_data_len_ ;

      // encode centroids
      if(this->do_voxel_centroid_enDecoding_)
      {
        // encode differential centroid information
        std::vector<char>& point_diff_data_vector = this->centroid_coder_.getDifferentialDataVector ();
		uint32_t point_diff_data_vector_size = (uint32_t) point_diff_data_vector.size();
        compressed_tree_data_out_arg1.write (reinterpret_cast<const char*> (&point_diff_data_vector_size), sizeof (uint32_t));
		this->compressed_point_data_len_ += this->entropy_coder_.encodeCharVectorToStream(point_diff_data_vector, compressed_tree_data_out_arg1);
      }

      // store the number of bytes used for voxel centroid encoding
      compression_performance_metrics[1] = this->compressed_point_data_len_ - compression_performance_metrics[0];

      // encode colors
      if (this->cloud_with_color_)
      {
        // encode averaged voxel color information
        std::vector<char>& pointAvgColorDataVector = color_coding_type_ ?  this->jp_color_coder_.getAverageDataVector () : this->color_coder_.getAverageDataVector ();
        point_avg_color_data_vector_size = pointAvgColorDataVector.size ();
        compressed_tree_data_out_arg1.write (reinterpret_cast<const char*> (&point_avg_color_data_vector_size), sizeof (point_avg_color_data_vector_size));
		this->compressed_color_data_len_ += this->entropy_coder_.encodeCharVectorToStream(pointAvgColorDataVector, compressed_tree_data_out_arg1);
      }

      // store the number of bytes used for the color stream
      compression_performance_metrics[2] = this->compressed_color_data_len_ ;

      // flush output stream
      compressed_tree_data_out_arg1.flush ();

      if (!this->do_voxel_grid_enDecoding_)
      {
        uint64_t pointCountDataVector_size;
        uint64_t point_diff_data_vector_size;
        uint64_t point_diff_color_data_vector_size;

        // encode amount of points per voxel
		pointCountDataVector_size = this->point_count_data_vector_.size();
        compressed_tree_data_out_arg2.write (reinterpret_cast<const char*> (&pointCountDataVector_size), 
					     sizeof (pointCountDataVector_size));
		this->compressed_point_data_len_ += this->entropy_coder_.encodeIntVectorToStream(this->point_count_data_vector_,
									      compressed_tree_data_out_arg2);

        // encode differential point information
		std::vector<char>& point_diff_data_vector = this->point_coder_.getDifferentialDataVector();
        point_diff_data_vector_size = point_diff_data_vector.size ();
        compressed_tree_data_out_arg2.write (reinterpret_cast<const char*> (&point_diff_data_vector_size), sizeof (point_diff_data_vector_size));
		this->compressed_point_data_len_ += this->entropy_coder_.encodeCharVectorToStream(point_diff_data_vector, compressed_tree_data_out_arg2);

        if (this->cloud_with_color_)
        {
          // encode differential color information
          std::vector<char>& point_diff_color_data_vector = this->color_coding_type_ ?  this->jp_color_coder_.getDifferentialDataVector () : this->color_coder_.getDifferentialDataVector ();
          point_diff_color_data_vector_size = point_diff_color_data_vector.size ();
          compressed_tree_data_out_arg2.write (reinterpret_cast<const char*> (&point_diff_color_data_vector_size),
					       sizeof (point_diff_color_data_vector_size));
          this->compressed_color_data_len_ += this->entropy_coder_.encodeCharVectorToStream (point_diff_color_data_vector,
										 compressed_tree_data_out_arg2);
        }
      }
      // flush output stream
      compressed_tree_data_out_arg2.flush ();
    };

    /** \brief Entropy decoding of input binary stream and output to information vectors, added bitstream scalability and centroid encoding compared to  V1
    * \param compressed_tree_data_in_arg: binary input stream1, binary input stream1,
    */
    template<typename PointT, typename LeafT, typename BranchT, typename OctreeT> void
      OctreePointCloudCodecV2<PointT, LeafT, BranchT, OctreeT>::entropyDecoding (std::istream& compressed_tree_data_in_arg1, std::istream& compressed_tree_data_in_arg2)
    {
      uint64_t binary_tree_data_vector_size;
      uint64_t point_avg_color_data_vector_size;

	  this->compressed_point_data_len_ = 0;
	  this->compressed_color_data_len_ = 0;

      // decode binary octree structure
      compressed_tree_data_in_arg1.read (reinterpret_cast<char*> (&binary_tree_data_vector_size), sizeof (binary_tree_data_vector_size));
	  this->binary_tree_data_vector_.resize(static_cast<std::size_t> (binary_tree_data_vector_size));
	  this->compressed_point_data_len_ += this->entropy_coder_.decodeStreamToCharVector(compressed_tree_data_in_arg1, this->binary_tree_data_vector_);

      //! new option for encoding centroids
      if(this->do_voxel_centroid_enDecoding_)
      {
        uint32_t l_count;

        // decode differential point information
        std::vector<char>& pointDiffDataVector = centroid_coder_.getDifferentialDataVector ();
        compressed_tree_data_in_arg1.read (reinterpret_cast<char*> (&l_count), sizeof (uint32_t));
        pointDiffDataVector.resize (static_cast<std::size_t> (l_count));
		this->compressed_point_data_len_ += this->entropy_coder_.decodeStreamToCharVector(compressed_tree_data_in_arg1, pointDiffDataVector);
      }

      if (this->data_with_color_)
      {
        // decode averaged voxel color information
		  std::vector<char>& point_avg_color_data_vector = this->color_coding_type_ ? jp_color_coder_.getAverageDataVector() : this->color_coder_.getAverageDataVector();
        compressed_tree_data_in_arg1.read (reinterpret_cast<char*> (&point_avg_color_data_vector_size), sizeof (point_avg_color_data_vector_size));
        point_avg_color_data_vector.resize (static_cast<std::size_t> (point_avg_color_data_vector_size));
		this->compressed_color_data_len_ += this->entropy_coder_.decodeStreamToCharVector(compressed_tree_data_in_arg1,
												point_avg_color_data_vector);
      }

      //! check if the enhancement layer has been received
      compressed_tree_data_in_arg2.peek();
      if(compressed_tree_data_in_arg2.good() && (! compressed_tree_data_in_arg2.eof()) )
        this->do_voxel_grid_enDecoding_ = false;
      else
        this->do_voxel_grid_enDecoding_ = true;

      if (!this->do_voxel_grid_enDecoding_)
      {
        uint64_t point_count_data_vector_size;
        uint64_t point_diff_data_vector_size;
        uint64_t point_diff_color_data_vector_size;

        // decode amount of points per voxel
        compressed_tree_data_in_arg2.read (reinterpret_cast<char*> (&point_count_data_vector_size), sizeof (point_count_data_vector_size));
        this->point_count_data_vector_.resize (static_cast<std::size_t> (point_count_data_vector_size));
		this->compressed_point_data_len_ += this->entropy_coder_.decodeStreamToIntVector(compressed_tree_data_in_arg2, this->point_count_data_vector_);
        this->point_count_data_vector_iterator_ = this->point_count_data_vector_.begin ();

        // decode differential point information
		std::vector<char>& pointDiffDataVector = this->point_coder_.getDifferentialDataVector();
        compressed_tree_data_in_arg2.read (reinterpret_cast<char*> (&point_diff_data_vector_size), sizeof (point_diff_data_vector_size));
        pointDiffDataVector.resize (static_cast<std::size_t> (point_diff_data_vector_size));
        this->compressed_point_data_len_ += this->entropy_coder_.decodeStreamToCharVector (compressed_tree_data_in_arg2,
											   pointDiffDataVector);
        if (this->data_with_color_)
        {
          // decode differential color information
          std::vector<char>& pointDiffColorDataVector = this->color_coder_.getDifferentialDataVector ();
          compressed_tree_data_in_arg2.read (reinterpret_cast<char*> (&point_diff_color_data_vector_size), sizeof (point_diff_color_data_vector_size));
          pointDiffColorDataVector.resize (static_cast<std::size_t> (point_diff_color_data_vector_size));
          this->compressed_color_data_len_ += this->entropy_coder_.decodeStreamToCharVector (compressed_tree_data_in_arg2,
											     pointDiffColorDataVector);
        }
      }
    };
  }
}
#endif