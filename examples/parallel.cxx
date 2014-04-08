#include "tree_mpi.h"
#include "args.h"
#include "bound_box.h"
#include "build_tree.h"
#include "dataset.h"
#include "logger.h"
#include "partition.h"
#include "traversal.h"
#include "up_down_pass.h"
#include "verify.h"
#if VTK
#include "vtk.h"
#endif

int main(int argc, char ** argv) {
  Args args(argc, argv);
  Bodies bodies, bodies2, jbodies, gbodies;
  BoundBox boundBox(args.nspawn);
  Bounds localBounds, globalBounds;
  BuildTree localTree(args.ncrit, args.nspawn);
  BuildTree globalTree(1, args.nspawn);
  Cells cells, jcells, gcells;
  Dataset data;
  Partition partition;
  Traversal traversal(args.nspawn, args.images);
  TreeMPI treeMPI(args.images);
  UpDownPass upDownPass(args.theta, args.useRmax, args.useRopt);
  Verify verify;

  const real_t cycle = 2 * M_PI;
  args.numBodies /= treeMPI.mpisize;
  args.verbose &= treeMPI.mpirank == 0;
  logger::verbose = args.verbose;
  logger::printTitle("FMM Parameters");
  args.print(logger::stringLength, P);
  logger::printTitle("FMM Profiling");
  logger::startTimer("Total FMM");
  logger::startPAPI();
  bodies = data.initBodies(args.numBodies, args.distribution, treeMPI.mpirank, treeMPI.mpisize);
  localBounds = boundBox.getBounds(bodies);
#if IneJ
  for (B_iter B=bodies.begin(); B!=bodies.end(); B++) {
    B->X[0] += M_PI;
    B->X[0] *= 0.5;
  }
  localBounds = boundBox.getBounds(bodies);
  jbodies = data.initBodies(args.numBodies, args.distribution, treeMPI.mpirank+treeMPI.mpisize, treeMPI.mpisize);
  for (B_iter B=jbodies.begin(); B!=jbodies.end(); B++) {
    B->X[0] -= M_PI;
    B->X[0] *= 0.5;
  }
  localBounds = boundBox.getBounds(jbodies,localBounds);
#endif
  globalBounds = treeMPI.allreduceBounds(localBounds);
  localBounds = partition.octsection(bodies,globalBounds);
  bodies = treeMPI.commBodies(bodies);
#if IneJ
  partition.octsection(jbodies,globalBounds);
  jbodies = treeMPI.commBodies(jbodies);
#endif
  localBounds = boundBox.getBounds(bodies);
  cells = localTree.buildTree(bodies, localBounds);
  upDownPass.upwardPass(cells);
#if IneJ
  localBounds = boundBox.getBounds(jbodies);
  jcells = localTree.buildTree(jbodies, localBounds);
  upDownPass.upwardPass(jcells);
#endif

#if 1 // Set to 0 for debugging by shifting bodies and reconstructing tree
  treeMPI.allgatherBounds(localBounds);
#if IneJ
  treeMPI.setLET(jcells, cycle);
#else
  treeMPI.setLET(cells, cycle);
#endif
  treeMPI.commBodies();
  treeMPI.commCells();
#if IneJ
  traversal.dualTreeTraversal(cells, jcells, cycle);
#else
  traversal.dualTreeTraversal(cells, cells, cycle, args.mutual);
  jbodies = bodies;
#endif
  for (int irank=0; irank<treeMPI.mpisize; irank++) {
    treeMPI.getLET(jcells,(treeMPI.mpirank+irank)%treeMPI.mpisize);
    traversal.dualTreeTraversal(cells, jcells, cycle);
  }
#if 0
  treeMPI.linkLET();
  gbodies = treeMPI.root2body();
  gcells = globalTree.buildTree(gbodies, globalBounds);
  treeMPI.attachRoot(gcells);
#endif
#else
  for (int irank=0; irank<treeMPI.mpisize; irank++) {
    treeMPI.shiftBodies(jbodies);
    jcells.clear();
    localBounds = boundBox.getBounds(jbodies);
    jcells = localTree.buildTree(jbodies, localBounds);
    upDownPass.upwardPass(jcells);
    traversal.dualTreeTraversal(cells, jcells, cycle, args.mutual);
  }
#endif
  upDownPass.downwardPass(cells);

  logger::stopPAPI();
  logger::stopTimer("Total FMM", 0);
  logger::printTitle("MPI direct sum");
  const int numTargets = 100;
  data.sampleBodies(bodies, numTargets);
  bodies2 = bodies;
  data.initTarget(bodies);
  logger::startTimer("Total Direct");
  for (int i=0; i<treeMPI.mpisize; i++) {
    if (args.verbose) std::cout << "Direct loop          : " << i+1 << "/" << treeMPI.mpisize << std::endl;
    treeMPI.shiftBodies(jbodies);
    traversal.direct(bodies, jbodies, cycle);
  }
  traversal.normalize(bodies);
  logger::printTitle("Total runtime");
  logger::printTime("Total FMM");
  logger::stopTimer("Total Direct");
  logger::resetTimer("Total FMM");
  logger::resetTimer("Total Direct");
#if WRITE_TIME
  logger::writeTime(treeMPI.mpirank);
#endif
  double potDif = verify.getDifScalar(bodies, bodies2);
  double potNrm = verify.getNrmScalar(bodies);
  double accDif = verify.getDifVector(bodies, bodies2);
  double accNrm = verify.getNrmVector(bodies);
  double potDifGlob, potNrmGlob, accDifGlob, accNrmGlob;
  MPI_Reduce(&potDif, &potDifGlob, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
  MPI_Reduce(&potNrm, &potNrmGlob, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
  MPI_Reduce(&accDif, &accDifGlob, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
  MPI_Reduce(&accNrm, &accNrmGlob, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
  logger::printTitle("FMM vs. direct");
  verify.print("Rel. L2 Error (pot)",std::sqrt(potDifGlob/potNrmGlob));
  verify.print("Rel. L2 Error (acc)",std::sqrt(accDifGlob/accNrmGlob));
  localTree.printTreeData(cells);
  traversal.printTraversalData();
  logger::printPAPI();

#if VTK
  for (B_iter B=jbodies.begin(); B!=jbodies.end(); B++) B->IBODY = 0;
  for (int irank=0; irank<treeMPI.mpisize; irank++) {
    treeMPI.getLET(jcells,(treeMPI.mpirank+irank)%treeMPI.mpisize);
    for (C_iter C=jcells.begin(); C!=jcells.end(); C++) {
      Body body;
      body.IBODY = 1;
      body.X     = C->X;
      body.SRC   = 0;
      jbodies.push_back(body);
    }
  }
  vtk3DPlot vtk;
  vtk.setBounds(M_PI,0);
  vtk.setGroupOfPoints(jbodies);
  for (int i=1; i<treeMPI.mpisize; i++) {
    treeMPI.shiftBodies(jbodies);
    vtk.setGroupOfPoints(jbodies);
  }
  if (treeMPI.mpirank == 0) {
    vtk.plot();
  }
#endif
  return 0;
}
