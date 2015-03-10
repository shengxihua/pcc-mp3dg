/*
* Software License Agreement (BSD License)
*
*  Point Cloud Library (PCL) - www.pointclouds.org
*  Copyright (c) 2014- Centrum Wiskunde Informatica
*
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
*   * Neither the name of its copyright holders nor the names of its
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

#include <pcl/compression/color_coding.h>
#include <pcl/cloud_codec_v2/snake_grid_mapping.h>
#include <pcl/io/jpeg_io.h>
#include <pcl/PCLImage.h>

namespace pcl{

  namespace octree{

    //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    /** \brief @b Octree Color Coding Based on the legacy JPEG coding via libJPEGTurbo
    *  \note This class encodes 8-bit color information via the libjpeg turbo in jpeg_io
    *  \note typename: PointT: type of point used in pointcloud
    *  \author Rufael Mekuria (rufael.mekuria@cwi.nl)
    */

    //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    // class that uses the JPEG codec to code the colors
    template<typename PointT>
    class ColorCodingJPEG : public  ColorCoding<PointT>
    {
    public :

      enum mappingTypes{SNAKE,LINES,GRID};

      //! JPEG lines data
      struct JPEGLineData
      {
        uint32_t line_count;
        std::vector<std::vector<uint8_t>> clines;
        // serialize to stream
        void 
        serialize(std::ostream &ss){
           // the size of the data will be 
           ss << (unsigned int)line_count; 
          
           for(int i;i<line_count;i++){
             ss << clines[i].size();
             ss.write((char *) clines[i].data(),clines[i].size());
           }
         }
        // read from stream
        void
        deserialize(std::istream &ss){
          ss >> line_count;
          clines.resize(line_count);
          for(int i=0; i< line_count;i++)
          {
            unsigned int line_size; 
            ss >> line_size;
            clines[i].resize(line_size);
            ss.read((char *) clines[i].data(),line_size);
          }
        }
      };

      ColorCodingJPEG (int quality = 75, int mode=LINES) 
        : ColorCoding<PointT>(),jpeg_quality_(quality), mapping_mode_(mode)
      {
        needs_jpeg_encoding_avg_ = false; 
      };

      /** \brief Empty class constructor. */
      virtual
      ~ColorCodingJPEG ()
      {
      };

      /** \brief Get reference to vector containing differential color data, only used by entropy coders, 
      so we pass only jpeg encoded data in this subclass
      * */
      virtual std::vector<char>&
      getAverageDataVector ()
      {
        // we still need to do jpeg encoding
        if(needs_jpeg_encoding_avg_){
          std::vector<uint8_t> out_data;
          switch(mapping_mode_)
          {
            case(mappingTypes::SNAKE):
              encodeJPEGSnake(pointAvgColorDataVector_, out_data);
              pointAvgColorDataVector_.resize(out_data.size());
              std::copy((char *) out_data.data(),(char *) out_data.data() + out_data.size(),(char *) pointAvgColorDataVector_.data());
            break;
            case(mappingTypes::LINES):
              encodeJPEGLines(pointAvgColorDataVector_, out_data);
              pointAvgColorDataVector_.resize(out_data.size());
              std::copy((char *) out_data.data(),(char *) out_data.data() + out_data.size(), (char *) pointAvgColorDataVector_.data());
            break;
            case(mappingTypes::GRID):
            break;
          }
        }
        return pointAvgColorDataVector_;
      };

      // function to just get the pointAvgVector
      virtual std::vector<char>&
      getAverageDataVectorB()
      {
        return pointAvgColorDataVector_;
      }

      /** \brief Initialize decoding of color information
      * */
      virtual void
      initializeDecoding()
      {
        std::vector<uint8_t> out_data;
        switch(mapping_mode_)
        {
          case(mappingTypes::SNAKE):
            decodeJPEGSnake(pointAvgColorDataVector_, out_data);
            pointAvgColorDataVector_Iterator_ = pointAvgColorDataVector_.begin();
          break;
          case(mappingTypes::LINES):
            decodeJPEGLines(pointAvgColorDataVector_, out_data);
            pointAvgColorDataVector_Iterator_ = pointAvgColorDataVector_.begin();
          break;
          case(mappingTypes::GRID):
          break;
          default:
          break;
        }

        pointAvgColorDataVector_Iterator_ = pointAvgColorDataVector_.begin ();
        pointDiffColorDataVector_Iterator_ = pointDiffColorDataVector_.begin ();
      }

      /** \brief Initialize encoding of color information
      * */
      virtual void
      initializeEncoding ()
      {
        pointAvgColorDataVector_.clear ();
        pointDiffColorDataVector_.clear ();

        needs_jpeg_encoding_avg_ = true;
      }
    
    public:
      
      void
      encodeJPEGSnake(std::vector<char> &in_vec, std::vector<uint8_t> & out_data)
      {
        // encode using the JPEG Snake
        PCLImage l_mapped_im;
          
        // compute the image grid 
        long pixel_count = in_vec.size()/3;

        // hardcoded value for horizonal width
        l_mapped_im.width = 256;
        l_mapped_im.height = pixel_count/l_mapped_im.width + 1;
        l_mapped_im.encoding = "RGB";
        long padded_pixels = l_mapped_im.width  * l_mapped_im.height - pixel_count;

        // color used for padding
        unsigned char last_color[3];
        last_color[0] =  in_vec[in_vec.size() -3];
        last_color[1] =  in_vec[in_vec.size() -2];
        last_color[2] =  in_vec[in_vec.size() -1];

        // pad using the last color
        for(int j=0;j<padded_pixels; j++ ){
          in_vec.push_back(last_color[0]);
          in_vec.push_back(last_color[1]);
          in_vec.push_back(last_color[2]);
        }

        //! map the colors to jpeg via a zigzag scan and code them as a single jpeg
        SnakeGridMapping<char,uint8_t> m(l_mapped_im.width ,l_mapped_im.height);
        std::vector<uint8_t> &res = m.doMapping(in_vec);
        l_mapped_im.data = std::move(res);
        io::JPEGWriter<uint8_t>::writeJPEG(l_mapped_im,out_data,jpeg_quality_);
        return;
      }

      void
      decodeJPEGSnake(std::vector<char> &in_vec, std::vector<uint8_t> & out_data)
      {
        PCLImage im_out; //!
        io::JPEGReader<char>::readJPEG(in_vec,im_out); //! 

        SnakeGridMapping<uint8_t,char> un_m(im_out.width,im_out.height);
        std::vector<char> res2 = un_m.undoSnakeGridMapping(im_out.data);
        in_vec = std::move(res2);
      }

      void
      encodeJPEGLines(std::vector<char> &in_vec, std::vector<uint8_t> & out_data)
      {
        // encode as lines of jpeg scans
        long pixel_count = in_vec.size()/3;
        int num_lines = pixel_count/2048;
        JPEGLineData cdat;
        cdat.line_count = num_lines;
        cdat.clines.resize(num_lines);

        // prepare mapped image of single lines
        PCLImage im_in;    
        im_in.width = 2048;
        im_in.height = 1;
        im_in.encoding = "RGB";

        if(num_lines == 0)
        {
          cdat.line_count = 1;
          cdat.clines.resize(1);
          std::vector<uint8_t> line_dat(3*pixel_count);
          std::copy((char *) in_vec.data(),
                    (char *) in_vec.data() + 3 * pixel_count, 
                    (char *) line_dat.data());
            
          im_in.data = std::move(line_dat);
          io::JPEGWriter<uint8_t>::writeJPEG(im_in,cdat.clines[0],jpeg_quality_);
        }
        //! write all the image lines to the grid
        for(int i =0; i< num_lines;i++)
        {
          if(i != num_lines -1 )
          {
            std::vector<uint8_t> line_dat(3*2048);
            std::copy((char *) in_vec.data() + 3 * 2048 * i,
                      (char *) in_vec.data() + 3 * 2048 * (i+1), 
                      (char *) line_dat.data());
            
            im_in.data = std::move(line_dat);
            io::JPEGWriter<uint8_t>::writeJPEG(im_in,cdat.clines[i],jpeg_quality_);
          }
          else
          {
            std::vector<uint8_t> line_dat(in_vec.size() - 3 * i *2048);
            std::copy((char *)   in_vec.data() + 3 * 2048 * i,
                      (char *)   in_vec.data() + in_vec.size(), 
                      (char *)   line_dat.data());

            im_in.data = std::move(line_dat);
            im_in.width = line_dat.size()/3;
            io::JPEGWriter<uint8_t>::writeJPEG(im_in,cdat.clines[i],jpeg_quality_);
          }
        }

        // copy the serialize buffer to the output vector
        stringstream l_outstr;
        cdat.serialize(l_outstr);
        out_data.resize(l_outstr.str().size() + 1);
        std::copy((char *)l_outstr.str().c_str(), (char *)l_outstr.str().c_str() + l_outstr.str().size() + 1 , (char *) out_data.data());
      }

      void
      decodeJPEGLines(std::vector<char> &in_vec, std::vector<uint8_t> & out_data)
      {
        //! collect the data and deserialize
        std::string data;
        data.resize(in_vec.size());
        std::copy((char *)in_vec.data(),(char *)in_vec.data() + in_vec.size(),(char *) data.c_str());
        stringstream l_instr(data);
        JPEGLineData cdat;
        cdat.deserialize(l_instr);

        out_data.clear();
        out_data.reserve(2048 * cdat.line_count);

        for(int i=0; i<cdat.line_count; i++)
        {
          PCLImage t_o_dat;
          io::JPEGReader<uint8_t>::readJPEG(cdat.clines[i],t_o_dat);
          for(int i=0; i<t_o_dat.data.size();i++)
            out_data.push_back(t_o_dat.data[i]);
        }
        
        // another copy to handle the difference between char and uint8_t
        pointAvgColorDataVector_.resize(out_data.size());
        std::copy((char *)out_data.data(),(char *)out_data.data() + out_data.size(), (char *) pointAvgColorDataVector_.data());
      }
      
      //! boolean to do jpeg encoding or not
      bool needs_jpeg_encoding_avg_;
      
      //! enum of mapping mode
      int  mapping_mode_;

      //! the quality parameter used for the compression
      int jpeg_quality_;
    };
  }
}