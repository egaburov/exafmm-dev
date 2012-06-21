/*
Copyright (C) 2011 by Rio Yokota, Simon Layton, Lorena Barba

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/
#ifndef parallelfmm_h
#define parallelfmm_h
#include "partition.h"

//! Handles all the communication of local essential trees
template<Equation equation>
class ParallelFMM : public Partition<equation> {
private:
  int IRANK;                                                    //!< MPI rank loop counter
  vect thisXMIN;                                                //!< XMIN for a given rank
  vect thisXMAX;                                                //!< XMAX for a given rank
  CellQueue cellQueue;                                          //!< Queue for tree traversal
  Cells sendCells;                                              //!< Send buffer for cells
  Cells recvCells;                                              //!< Receive buffer for cells
  int *sendCellCount;                                           //!< Send count
  int *sendCellDispl;                                           //!< Send displacement
  int *recvCellCount;                                           //!< Receive count
  int *recvCellDispl;                                           //!< Receive displacement

public:
  using Kernel<equation>::printNow;                             //!< Switch to print timings
  using Kernel<equation>::startTimer;                           //!< Start timer for given event
  using Kernel<equation>::stopTimer;                            //!< Stop timer for given event
  using Kernel<equation>::X0;                                   //!< Center of root cell
  using Kernel<equation>::R0;                                   //!< Radius of root cel
  using Kernel<equation>::Cj0;                                  //!< Begin iterator for source cells
  using Evaluator<equation>::TOPDOWN;                           //!< Flag for top down tree construction
  using Partition<equation>::print;                             //!< Print in MPI
  using Partition<equation>::XMIN;                              //!< Minimum position vector of bodies
  using Partition<equation>::XMAX;                              //!< Maximum position vector of bodies
  using Partition<equation>::sendBodies;                        //!< Send buffer for bodies
  using Partition<equation>::recvBodies;                        //!< Receive buffer for bodies
  using Partition<equation>::sendBodyCount;                     //!< Send count
  using Partition<equation>::sendBodyDispl;                     //!< Send displacement
  using Partition<equation>::recvBodyCount;                     //!< Receive count
  using Partition<equation>::recvBodyDispl;                     //!< Receive displacement
  using Partition<equation>::alltoall;                          //!< Exchange send count for bodies
  using Partition<equation>::alltoallv;                         //!< Exchange bodies

private:
//! Get disatnce to other domain
  real getDistance(C_iter C) {
    vect dist;                                                  // Distance vector
    for( int d=0; d!=3; ++d ) {                                 // Loop over dimensions
      dist[d] = (C->X[d] + Xperiodic[d] > thisXMAX[d])*         //  Calculate the distance between cell C and
                (C->X[d] + Xperiodic[d] - thisXMAX[d])+         //  the nearest point in domain [xmin,xmax]^3
                (C->X[d] + Xperiodic[d] < thisXMIN[d])*         //  Take the differnece from xmin or xmax
                (C->X[d] + Xperiodic[d] - thisXMIN[d]);         //  or 0 if between xmin and xmax
    }                                                           // End loop over dimensions
    real R = std::sqrt(norm(dist));                             // Scalar distance
    return R;                                                   // Return scalar distance
  }

//! Add cells to send buffer
  void addSendCell(C_iter C, int &iparent, int &icell) {
    Cell cell(*C);                                              // Initialize send cell
    cell.NCHILD = cell.NCLEAF = cell.NDLEAF = 0;                // Reset counters
    cell.PARENT = iparent;                                      // Iterator offset for parent
    sendCells.push_back(cell);                                  // Push to send cell vector
    icell++;                                                    // Increment cell counter
    C_iter Cparent = sendCells.begin() + iparent + sendCellDispl[IRANK];// Get parent iterator
    if( Cparent->NCHILD == 0 ) Cparent->CHILD = icell;          // Iterator offset for parent's first child
    Cparent->NCHILD++;                                          // Increment parent's child counter
  }

//! Add bodies to send buffer
  void addSendBody(C_iter C, int &ibody, int icell) {
    C_iter Csend = sendCells.begin() + icell + sendCellDispl[IRANK];// Get send cell iterator
    Csend->NCLEAF = C->NCLEAF;                                  // Set number of bodies
    Csend->NDLEAF = ibody;                                      // Set body index per rank
    for( B_iter B=C->LEAF; B!=C->LEAF+C->NCLEAF; ++B ) {        // Loop over bodies in cell
      sendBodies.push_back(*B);                                 //  Push to send body vector
      sendBodies.back().IPROC = IRANK;                          //  Set current rank
    }                                                           // End loop over bodies in cell
    ibody+=C->NCLEAF;                                           // Increment body counter
  }

//! Determine which cells to send
  void traverseLET() {
    int ibody = 0;                                              // Current send body's offset
    int icell = 0;                                              // Current send cell's offset
    int iparent = 0;                                            // Parent send cell's offset
    int level = int(log(MPISIZE-1) / M_LN2 / 3) + 1;            // Level of local root cell
    if( MPISIZE == 1 ) level = 0;                               // Account for serial case
    while( !cellQueue.empty() ) {                               // While traversal queue is not empty
      C_iter C = cellQueue.front();                             //  Get front item in traversal queue
      cellQueue.pop();                                          //   Pop item from traversal queue
      for( C_iter CC=Cj0+C->CHILD; CC!=Cj0+C->CHILD+C->NCHILD; ++CC ) {// Loop over child cells
        addSendCell(CC,iparent,icell);                          //   Add cells to send
        if( CC->NCHILD == 0 ) {                                 //   If cell is twig
          addSendBody(CC,ibody,icell);                          //    Add bodies to send
        } else {                                                //   If cell is not twig
          bool divide = false;                                  //    Initialize logical for dividing
          if( IMAGES == 0 ) {                                   //    If free boundary condition
            Xperiodic = 0;                                      //     Set periodic coordinate offset
            real R = getDistance(CC);                           //     Get distance to other domain
            divide |= 2 * CC->R > THETA * R;                    //     Divide if the cell seems too close
          } else {                                              //    If periodic boundary condition
            for( int ix=-1; ix<=1; ++ix ) {                     //     Loop over x periodic direction
              for( int iy=-1; iy<=1; ++iy ) {                   //      Loop over y periodic direction
                for( int iz=-1; iz<=1; ++iz ) {                 //       Loop over z periodic direction
                  Xperiodic[0] = ix * 2 * R0;                   //        Coordinate offset for x periodic direction
                  Xperiodic[1] = iy * 2 * R0;                   //        Coordinate offset for y periodic direction
                  Xperiodic[2] = iz * 2 * R0;                   //        Coordinate offset for z periodic direction
                  real R = getDistance(CC);                     //        Get distance to other domain
                  divide |= 2 * CC->R > THETA * R;              //        Divide if cell seems too close
                }                                               //       End loop over z periodic direction
              }                                                 //      End loop over y periodic direction
            }                                                   //     End loop over x periodic direction
          }                                                     //    Endif for periodic boundary condition
          divide |= CC->R > (R0 / (1 << level));                //    Divide if cell is larger than local root cell
          if( divide ) {                                        //    If cell has to be divided
            cellQueue.push(CC);                                 //     Push cell to traveral queue
          }                                                     //    Endif for cell division
        }                                                       //   Endif for twig
      }                                                         //  End loop over child cells
      iparent++;                                                //  Increment parent send cell's offset
    }                                                           // End while loop for traversal queue
  }

//! Exchange send count for cells
  void alltoall(Cells) {
    MPI_Alltoall(sendCellCount,1,MPI_INT,                       // Communicate send count to get receive count
                 recvCellCount,1,MPI_INT,MPI_COMM_WORLD);
    recvCellDispl[0] = 0;                                       // Initialize receive displacements
    for( int irank=0; irank!=MPISIZE-1; ++irank ) {             // Loop over ranks
      recvCellDispl[irank+1] = recvCellDispl[irank] + recvCellCount[irank];//  Set receive displacement
    }                                                           // End loop over ranks
  }

//! Exchange cells
  void alltoallv(Cells &cells) {
    int word = sizeof(cells[0]) / 4;                            // Word size of body structure
    recvCells.resize(recvCellDispl[MPISIZE-1]+recvCellCount[MPISIZE-1]);// Resize receive buffer
    for( int irank=0; irank!=MPISIZE; ++irank ) {               // Loop over ranks
      sendCellCount[irank] *= word;                             //  Multiply send count by word size of data
      sendCellDispl[irank] *= word;                             //  Multiply send displacement by word size of data
      recvCellCount[irank] *= word;                             //  Multiply receive count by word size of data
      recvCellDispl[irank] *= word;                             //  Multiply receive displacement by word size of data
    }                                                           // End loop over ranks
    MPI_Alltoallv(&cells[0],sendCellCount,sendCellDispl,MPI_INT,// Communicate cells
                  &recvCells[0],recvCellCount,recvCellDispl,MPI_INT,MPI_COMM_WORLD);
    for( int irank=0; irank!=MPISIZE; ++irank ) {               // Loop over ranks
      sendCellCount[irank] /= word;                             //  Divide send count by word size of data
      sendCellDispl[irank] /= word;                             //  Divide send displacement by word size of data
      recvCellCount[irank] /= word;                             //  Divide receive count by word size of data
      recvCellDispl[irank] /= word;                             //  Divide receive displacement by word size of data
    }                                                           // End loop over ranks
  }

public:
  ParallelFMM() {
    sendCellCount = new int [MPISIZE];                          // Allocate send count
    sendCellDispl = new int [MPISIZE];                          // Allocate send displacement
    recvCellCount = new int [MPISIZE];                          // Allocate receive count
    recvCellDispl = new int [MPISIZE];                          // Allocate receive displacement
  }
//! Destructor
  ~ParallelFMM() {
    delete[] sendCellCount;                                     // Deallocate send count
    delete[] sendCellDispl;                                     // Deallocate send displacement
    delete[] recvCellCount;                                     // Deallocate receive count
    delete[] recvCellDispl;                                     // Deallocate receive displacement
  }

//! Get local essential tree to send to each process
  void getLET(Cells &cells) {
    startTimer("Get LET");                                      // Start timer
    Cj0 = cells.begin();                                        // Set cells begin iterator
    C_iter Root;                                                // Root cell
    if( TOPDOWN ) {                                             // If tree was constructed top down
      Root = cells.begin();                                     //  The first cell is the root cell
    } else {                                                    // If tree was constructed bottom up
      Root = cells.end() - 1;                                   //  The last cell is the root cell
    }                                                           // Endif for tree construction
    sendCells.reserve(cells.size()*27);                         // Reserve space for send buffer
    sendCellDispl[0] = 0;                                       // Initialize displacement vector
    for( IRANK=0; IRANK!=MPISIZE; ++IRANK ) {                   // Loop over ranks
      if( IRANK != 0 ) sendCellDispl[IRANK] = sendCellDispl[IRANK-1] + sendCellCount[IRANK-1];// Update displacement
      if( IRANK != MPIRANK ) {                                  //  If not current rank
        thisXMIN = XMIN[IRANK];                                 //   Set XMIN for IRANK
        thisXMAX = XMAX[IRANK];                                 //   Set XMAX for IRANK
        Cell cell(*Root);                                       //   Send root cell
        cell.NCHILD = cell.NCLEAF = cell.NDLEAF = 0;            //   Reset link to children and leafs
        sendCells.push_back(cell);                              //   Push it into send buffer
        cellQueue.push(Root);                                   //   Push root to traversal queue
        traverseLET();                                          //   Traverse tree to get LET
      }                                                         //  Endif for current rank
      sendCellCount[IRANK] = sendCells.size() - sendCellDispl[IRANK];// Send count for IRANK
    }                                                           // End loop over ranks
    stopTimer("Get LET",printNow);                              // Stop timer
  }

//! Set local essential tree from rank "irank".
  void setLET(Cells &cells, int irank) {
    startTimer("Set LET");                                      // Start timer
    for( int i=recvCellDispl[irank]+recvCellCount[irank]-1; i>=recvCellDispl[irank]; i-- ) { // Loop over receive cells
      C_iter C = recvCells.begin() + i;                         //  Iterator for receive cell
      if( C->NCLEAF != 0 ) {                                    //  If cell has leafs
        C->LEAF = recvBodies.begin() + C->NDLEAF + recvBodyDispl[irank];// Iterator of first leaf
        C->NDLEAF = C->NCLEAF;                                  //   Initialize number of leafs
      }                                                         //  End if for leafs
      if( C->NCHILD != 0 ) C->CHILD += recvCellDispl[irank];    //  Add receive rank offset to child index
      if( i != recvCellDispl[irank] ) {                         //  If cell is not root
        C->PARENT += recvCellDispl[irank];                      //   Add receive rank offset to parent index
        C_iter Cparent = recvCells.begin() + C->PARENT;         //   Iterator for parent cell
        Cparent->NDLEAF += C->NDLEAF;                           //   Accululate number of leafs
      }                                                         //  End if for root cell
    }                                                           // End loop over receive cells
    cells.resize(recvCellCount[irank]);                         // Resize cell vector for LET
    cells.assign(recvCells.begin()+recvCellDispl[irank],recvCells.begin()+recvCellDispl[irank]+recvCellCount[irank]);
    stopTimer("Set LET",printNow);                              // Stop timer
  }

//! Send bodies
  void commBodies() {
    startTimer("Comm bodies");                                  // Start timer
    alltoall(sendBodies);                                       // Send body count
    alltoallv(sendBodies);                                      // Send bodies
    stopTimer("Comm bodies",printNow);                          // Stop timer
  }

//! Send cells
  void commCells() {
    startTimer("Comm cells");                                   // Start timer
    alltoall(sendCells);                                        // Send cell count
    alltoallv(sendCells);                                       // Senc cells
    stopTimer("Comm cells",printNow);                           // Stop timer
  }

};
#endif
