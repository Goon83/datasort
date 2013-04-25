#include "sortio.h"

// --------------------------------------------------------------------
// InitRead(): initialize threading environment for IO read tasks
// --------------------------------------------------------------------

void sortio_Class::Init_Read()
{
  assert(initialized_);

  // This routine is only meaningful on IO_tasks

  if(!isIOTask_ && (sortMode_ > 0) )
    return;

  gt.BeginTimer("Init Read");

  // Initialize read buffers - todo: think about memory pinning here

  if(sortMode_ <= 0)
    {
      size_t bufSize = MAX_READ_BUFFERS*MAX_FILE_SIZE_IN_MBS*1000*1000/sizeof(sortRecord);
      readBuf_.resize(bufSize);
    }
  else
    {
      buffers_.resize(MAX_READ_BUFFERS);

      for(int i=0;i<MAX_READ_BUFFERS;i++)
	{
	  buffers_[i] = (unsigned char*) calloc(MAX_FILE_SIZE_IN_MBS*1024*1024,sizeof(unsigned char));
	  assert(buffers_[i] != NULL);
	  
	  // Flag buffer as being eligible to receive data
	  
	  emptyQueue_.push_back(i);
	}
    }

  if(isMasterIO_)
    {
      grvy_printf(INFO,"[sortio]\n");
      grvy_printf(INFO,"[sortio][IO] %i Read buffers allocated (%i MB each)\n",
		  MAX_READ_BUFFERS,MAX_FILE_SIZE_IN_MBS);
    }

  gt.EndTimer("Init Read");

  // Initialize and launch threading environment for an asychronous
  // data transfer mechanism

  if(sortMode_ <= 0)		// no threading necessary in read-only mode
    {
      ReadFiles();
      return;
    }

  const int num_io_threads_per_host = 2;
  omp_set_num_threads(num_io_threads_per_host);

#pragma omp parallel
#pragma omp sections
  {
    #pragma omp section		// MPI transfer thread
    {
      Transfer_Tasks_Work();
    }

    #pragma omp section		// Read thread
    {
      IO_Tasks_Work();
    }
  }

  return;
}

// --------------------------------------------------------------------
// ReadFiles(): primary routine for reading raw sort data
//
// * Operates on IO_COMM communicator
// * Threading - data is buffered from thread 1 to thread 0 as it is 
//               read for subsequent distribution to sort tasks.
// --------------------------------------------------------------------

void sortio_Class::ReadFiles()
{

  assert(initialized_);
  assert(isIOTask_);

  unsigned long records_per_file;

  gt.BeginTimer("Raw Read");
  int num_iters = (numFilesTotal_+numIoTasks_-1)/numIoTasks_;
  size_t read_size;

  std::string filebase(inputDir_);
  filebase += "/";
  filebase += fileBaseName_;

  for(int iter=0;iter<num_iters;iter++)
    {

      std::ostringstream s_id;
      int file_suffix = iter*numIoTasks_ + ioRank_;

      // Optionally randomize so we can minimize local host
      // file-caching for more legitimate reads; the idea here is to
      // help enable repeat testing on the same hosts for smaller
      // dataset sizes. We just keep a local file here and iterate an
      // offset on each run

      if(random_read_offset_)
	{
#if 0
	  int min_index = iter*numIoTasks_;
	  int max_index = iter*numIoTasks_ + (numIoTasks_-1);
#endif
	  int offset;

	  if(master)
	    {
	      FILE *fp_offset = fopen(".offset.tmp","r");
	      if(fp_offset != NULL)
		{
		  fscanf(fp_offset,"%i",&offset);
		  offset += 17;
		  fclose(fp_offset);
		}
	      else
		offset = 1;

	      if(offset > numIoTasks_)
		offset = 0;

	      fp_offset = fopen(".offset.tmp","w");
	      assert(fp_offset != NULL);
	      fprintf(fp_offset,"%i\n",offset);
	      fclose(fp_offset);

#if 0
	      printf("min_index = %4i\n",min_index);
	      printf("max_index = %4i\n",max_index);
	      printf("offset    = %4i\n",offset);
#endif
	    }

	  MPI_Bcast(&offset,1,MPI_INTEGER,0,IO_COMM);

#if 0
	  printf("[sortio][%3i]: original file_suffix = %3i\n",ioRank_,file_suffix);
#endif
	  file_suffix += offset;

	  if(file_suffix > (iter*numIoTasks_ + (numIoTasks_-1)) )
	    file_suffix -= numIoTasks_;

#if 0
	  printf("[sortio][%3i]: new file_suffix = %3i\n",ioRank_,file_suffix);
#endif
	} // end if(random_read_offset)

      if(file_suffix >= numFilesTotal_)
	{
	  break;
	  //gt.EndTimer("Raw Read");
	  //return;
	}

      s_id << file_suffix;
      std::string infile = filebase + s_id.str();

      grvy_printf(INFO,"[sortio][IO/Read][%.4i]: filename = %s\n",ioRank_,infile.c_str());

      FILE *fp = fopen(infile.c_str(),"r");
      
      if(fp == NULL)
	MPI_Abort(MPI_COMM_WORLD,42);

      // pick available buffer for data storage; buffer is a
      // convenience pointer here which is set to empty data storage
      // prior to each raw read

      int buf_num = 0;
      unsigned char *buffer;	

      // Stall briefly if no empty queue buffers are available

      if(emptyQueue_.size() == 0 && sortMode_ > 0)
	for(int i=0;i<500000;i++)
	  {
	    grvy_printf(INFO,"[sortio][IO/Read][%.4i] no empty buffers, stalling....\n",ioRank_);
	    usleep(100000);
	    if(emptyQueue_.size() > 0)
	      break;
	  }

      if(sortMode_ <= 0)
	{
	  buf_num++;
	}
      else
	{
#pragma omp critical (IO_XFER_UPDATES_lock) // Thread-safety: all queue updates are locked
	  {
	    grvy_printf(DEBUG,"[sortio][IO/Read][%.4i]: # Empty buffers = %2i\n",ioRank_,emptyQueue_.size());
	    assert(emptyQueue_.size() > 0);
	    buf_num = emptyQueue_.front();
	    emptyQueue_.pop_front();
	  }
	}

      assert(buf_num < MAX_READ_BUFFERS);

      // initialize for next file read

      if(sortMode_ > 0)
	buffer         = buffers_[buf_num];

      int expectedSize;
      records_per_file = 0;

      if(sortMode_ <= 0)
	{
	  read_size    = 1;
	  expectedSize = 1;
	}
      else
	{
	  read_size    = REC_SIZE;
	  expectedSize = REC_SIZE;
	}

      // read till end of file (we assume file is even multiple of REC_SIZE)

      const int MAX_RETRIES = 5;
      int num_retries = 0;


      //      if(isFirstRead_)
      if(true)
	{
	  while(read_size == expectedSize)
	    {
	      // todo: test a blocked read here, say 100, 1000, 10000, etc REC_SIZEs

	      if(sortMode_ < 0)
		read_size = fread(&readBuf_[numRecordsRead_],sizeof(sortRecord),1,fp);
	      else if(sortMode_ == 0)
		read_size = fread(&readBuf_[0],sizeof(sortRecord),1,fp);
	      else
		read_size = fread(&buffer[records_per_file*REC_SIZE],1,REC_SIZE,fp);
	      
	      if(read_size == 0)
		break;
	     
	      if(sortMode_ <= 0)
		assert(read_size == 1);
	      else
		assert(read_size == REC_SIZE);

	      numRecordsRead_++;
	      records_per_file++;
	    }
	}
      else
	{
	  if(sortMode_ < 0)
	    records_per_file = fread(&readBuf_[numRecordsRead_],sizeof(sortRecord),recordsPerFile_,fp);
	  else if(sortMode_ == 0)
	    records_per_file = fread(&readBuf_[0],sizeof(sortRecord),recordsPerFile_,fp);
	  else
	    records_per_file = fread(&buffer[0],REC_SIZE,recordsPerFile_,fp);

	  numRecordsRead_ += recordsPerFile_;
	}

      fclose(fp);

      if(isFirstRead_)
	{
	  recordsPerFile_ = records_per_file;
	  isFirstRead_ = false;
	}

      // we assume for now, that all files are equal in size

      if(records_per_file != recordsPerFile_)
	{
	  grvy_printf(ERROR,"[sortio][IO/Read][%.4i] unexpected file read encountered (%i records vs %i expected)\n",
		      ioRank_,records_per_file,recordsPerFile_);
	  grvy_printf(ERROR,"[sortio][IO/Read][%.4i] filename = %s, buffer num = %i\n",
		      ioRank_,infile.c_str(),buf_num);
	}

      if(sortMode_ > 0)
	{
#pragma omp critical (IO_XFER_UPDATES_lock) // Thread-safety: all queue updates are locked
	  {
	    fullQueue_.push_back(buf_num);
	    grvy_printf(INFO,"[sortio][IO/Read][%.4i]: # Full buffers  = %2i\n",ioRank_,fullQueue_.size());
	  }
	}

      grvy_printf(DEBUG,"[sortio][IO/Read][%.4i]: records read = %i\n",ioRank_,records_per_file);

    } // end read iteration loop

  isReadFinished_ = true;

  gt.EndTimer("Raw Read");

  if(sortMode_ != -1)
    return;

  // do sort here if we are in naive mode

  MPI_Barrier(IO_COMM);

  if(master)
    grvy_printf(INFO,"[sortio][NAIVESORT] Starting sort process....\n"); 


  readBuf_.resize( numRecordsRead_ );
  std::vector<sortRecord> out;

  if(verifyMode_ > 0 )
    {
      if(master)
	grvy_printf(INFO,"[sortio][NAIVESORT] - dumping input data...\n");

      char filename[1024];
      sprintf(filename,"./partfromread_%i",ioRank_);
      FILE *fp = fopen(filename,"wb");
      assert(fp != NULL);
	      
      fwrite(&readBuf_[0],sizeof(sortRecord),readBuf_.size(),fp);
      fclose(fp);
    }

      //grvy_printf(INFO,"[sortio][NAIVESORT] Calling final sort with input size = %zi\n",readBuf_.size());

  //omp_par::merge_sort(&readBuf_[0],&readBuf_[readBuf_.size()]);

  gt.BeginTimer("Naive - QuickSort");
  par::HyperQuickSort_kway(readBuf_, out, GLOB_COMM);
  gt.EndTimer("Naive - QuickSort");

  if(master)
    grvy_printf(INFO,"[sortio][NAIVESORT] Finished sort\n");

  readBuf_.clear();

  gt.BeginTimer("Final Write");	  

  char tmpFilename[1024];	     
  sprintf(tmpFilename,"%s/part_bin_p%.5i",outputDir_.c_str(),ioRank_);
  grvy_check_file_path(tmpFilename);
		  
  FILE *fp = fopen(tmpFilename,"wb");
  assert(fp != NULL);
		  
  fwrite(&out[0],sizeof(sortRecord),out.size(),fp);
  fclose(fp);
  gt.EndTimer("Final Write");	  


  return;

}
