#include <iostream>
#include <fstream>
#include <getopt.h>
#include <cmath>
#include <cassert>
#define ASSERT assert

#include <particle_simulator.hpp>
#include "dyn_tide_eqs.hpp"
#include "../src/io.hpp"

//! A sample particle class
/*! A particle class should contain public members:
  Float mass, Float pos[3], Float vel[3], 
*/
class Particle{
public:
    long long int id;
    Float mass;
    PS::F64vec3 pos;
    PS::F64vec3 vel;

    //! write class data to file with binary format
    /*! @param[in] _fp: FILE type file for output
     */
    void writeBinary(FILE *_fout) const {
        fwrite(this, sizeof(*this),1,_fout);
    }


    //! read class data to file with binary format
    /*! @param[in] _fp: FILE type file for reading
     */
    void readBinary(FILE *_fin) {
        size_t rcount = fread(this, sizeof(*this),1,_fin);
        if (rcount<1) {
            std::cerr<<"Error: Data reading fails! requiring data number is 1, only obtain "<<rcount<<".\n";
            abort();
        }
    }


    //! write class data to file with ASCII format
    /*! @param[in] _fout: std:osteram file for output
     */
    void writeAscii(std::ostream& _fout) const {
        _fout<<mass<<" "
             <<pos[0]<<" "
             <<pos[1]<<" " 
             <<pos[2]<<" " 
             <<vel[0]<<" " 
             <<vel[1]<<" " 
             <<vel[2]<<" ";
    }

    //! read class data to file with ASCII format
    /*! @param[in] _fin: std::istream file for input
     */
    void readAscii(std::istream&  _fin) {
        _fin>>mass>>pos[0]>>pos[1]>>pos[2]>>vel[0]>>vel[1]>>vel[2];
        id = 1;
    }
    
    //! print titles of class members using column style
    /*! print titles of class members in one line for column style
      @param[out] _fout: std::ostream output object
      @param[in] _width: print width (defaulted 20)
    */
    static void printColumnTitle(std::ostream & _fout, const int _width=20) {
        _fout<<std::setw(_width)<<"mass"
             <<std::setw(_width)<<"pos.x"
             <<std::setw(_width)<<"pos.y"
             <<std::setw(_width)<<"pos.z"
             <<std::setw(_width)<<"vel.x"
             <<std::setw(_width)<<"vel.y"
             <<std::setw(_width)<<"vel.z";
    }

    //! print data of class members using column style
    /*! print data of class members in one line for column style. Notice no newline is printed at the end
      @param[out] _fout: std::ostream output object
      @param[in] _width: print width (defaulted 20)
    */
    void printColumn(std::ostream & _fout, const int _width=20){
        _fout<<std::setw(_width)<<mass
             <<std::setw(_width)<<pos[0]
             <<std::setw(_width)<<pos[1]
             <<std::setw(_width)<<pos[2]
             <<std::setw(_width)<<vel[0]
             <<std::setw(_width)<<vel[1]
             <<std::setw(_width)<<vel[2];
    }
    
};

int main(int argc, char** argv){

    Float G = 0.00449830997959438;
    Float poly_type = 1.5;
    int width = WRITE_WIDTH;
    int precision = WRITE_PRECISION;

    auto printHelp= [&]() {
        std::cout<<"The tool to test dynamical tide effect on hyperbolic orbit"<<std::endl;
        std::cout<<"Usage: petar.dyn.tide.test [options] [orbital data file]\n"
                 <<"       [orbital data file]: A file contain a list of binaries\n"
                 <<"       First line contains one value: number of orbits\n"
                 <<"       Following lines contain one orbit per line:\n";
        COMM::BinaryTree<Particle,COMM::Binary>::printColumnTitle(std::cout, 12);
        std::cout<<"\nOptions: \n"
                 <<"    -G [F]: gravitational constant ("<<G<<")\n"
                 <<"    -t [F]: polynomial types (1.5 or 3.0) ("<<poly_type<<")\n"
                 <<"    -w [I]: print column width ("<<width<<")\n"
                 <<"    -p [I]: print precision ("<<precision<<")\n"
                 <<"    -h    : help\n";
    };

    // reset optind
    static int long_flag=-1;
    static struct option long_options[] = {
        {"help",     no_argument,       0, 'h'},
        {0,0,0,0}
    };

    int arg_label;
    int opt_used = 0;
    optind=0;
    int option_index;
    while ((arg_label = getopt_long(argc, argv, "w:p:t:G:h", long_options, &option_index)) != -1)
        switch (arg_label) {
        case 0:
            switch (long_flag) {
            default:
                break;
            }
            break;
        case 'w':
            width = atoi(optarg);
            std::cout<<"print width: "<<width<<std::endl;
            opt_used+=2;
            break;
        case 'p':
            precision=atoi(optarg);
            std::cout<<"print precision: "<<precision<<std::endl;
            opt_used+=2;
            break;
        case 'G':
            G = atof(optarg);
            std::cout<<"gravitational constant: "<<G<<std::endl;
            opt_used+=2;
            break;
        case 't':
            poly_type = atof(optarg);
            std::cout<<"polynomial type: "<<poly_type<<std::endl;
            opt_used+=2;
            break;
        case 'h':
            printHelp();
            break;
        case '?':
            opt_used +=2;
            break;
        default:
            break;
        }        
    
   if (argc==1) {
       std::cerr<<"Please provide particle data filename\n";
       abort();
   }

   std::cout<<std::setprecision(precision);

    
   // data file name
   char* filename = argv[argc-1];
   
   // open data file
   std::fstream fs;
   fs.open(filename,std::fstream::in);
   if(!fs.is_open()) {
       std::cerr<<"Error: Filename "<<filename<<" not found\n";
       abort();
   }

   int num=0;
   fs>>num;
   assert(num>0);

   Particle p[2];
   COMM::BinaryTree<Particle,COMM::Binary> bin;
   bin.setMembers(&p[0],&p[1],1,2);
   
   DynamicTide dyn_tide;
   dyn_tide.gravitational_constant = G;
   dyn_tide.poly_type = poly_type;

   for(int i=0; i<num; i++) {
       bin.readAscii(fs);
       Float rad1,rad2;
       fs>>rad1>>rad2;
       bin.calcParticles(G);
       
       dyn_tide.evolveOrbit(bin, rad1, rad2);
       
   }   

   return 0;
}

