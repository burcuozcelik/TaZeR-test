#!/bin/bash

#This test uses File_access_pattern_gen.py and workloadSim.cpp, it takes cache sizes as arguments as well as
#arguments for File_access_pattern_gen.py.

workspace=$1 
data_path=$2
tazer_path=$3
build_dir=$4
server_addr=$5
server_port=$6
test_id=$7
sim_dir=$8
sim_params="$9"
#cache params $10 11 12
ref_time=${13}
mkdir -p ${workspace}/test_${test_id}
cd ${workspace}/test_${test_id}

{
    MY_HOSTNAME=`hostname`
    ulimit -n 4096
    echo "#### EXECUTING TAZER WORKFLOW TEST ($TEST_ID) on $MY_HOSTNAME"
    echo "$@"

    TAZER_LIB=${tazer_path}/${build_dir}/src/client/libclient.so

    #This checks arguments $10 $11 $12, if they are 0 then the corresponding cache is not used, if they are not 0
    #then they are assumed to be the cache size of the corresponding cache.
    CACHES=(0 0 0)
    CACHE_SIZES=(0 0 0)
    x=0
    for i in ${10} ${11} ${12}
    do
        if [ ! $i == 0 ]; then
            CACHES[$x]=1
            CACHE_SIZES[$x]=$i
        fi    
        x=$(($x+1))
    done

    SERVER_DATA_PATH=$data_path
    LOCAL_DATA_PATH=./data #relative to where the client executes
    mkdir -p $LOCAL_DATA_PATH

    echo "### Creating tazer meta files"
    compression=0
    blocksize=1048576
    infile="$SERVER_DATA_PATH/tazer1GB.dat"

    echo "${server_addr}:${server_port}:${compression}:0:0:${blocksize}:${infile}|" | tee ${LOCAL_DATA_PATH}/tazer1GB.dat.meta.in
    out_file=./log.out

    echo "python3 ${sim_dir}/File_access_pattern_gen.py $sim_params --inputFileName=${infile} --outputFileName=access_pattern.txt --plot=access_pattern.png"
    x=$(python3 ${sim_dir}/File_access_pattern_gen.py $sim_params --inputFileName=${infile} --outputFileName=access_pattern.txt --plot=access_pattern.png)
    echo "$x"
    if [ ! $? == 0 ]; then
        echo "$0 failed"
        exit 1
    fi

    #Run workloadSim without tazer server. The "--calculatehash only" will make it run fast and not go through the full simulation.
    $sim_dir/workloadSim --infile access_pattern.txt --calculatehash only --iorate $x
    #Copy the hash.txt to hash2.txt for the comparison test at the end.
    cp hash.txt hash2.txt
    #Remove ${SERVER_DATA_PATH} from the beginning of each line of the access_pattern.txt and replace with data/ so that it looks for
    #the meta files in the local data directory when the "--infilesuffix .meta.in" option is used.
    sed -i 's+'${SERVER_DATA_PATH}'/+data/+g' access_pattern.txt
    #Run workloadSim with tazer using the given cache sizes.
    
    ref_time=0 #debug -- calcuate a reference time (e.g. with SimplePTP if you want to use)
    echo "ref_time ${ref_time}" 
    # time gdb --ex run --ex bt --args env  
    time TAZER_REF_TIME=${ref_time} TAZER_SHARED_MEM_CACHE=$((${CACHES[1]}))  TAZER_SHARED_MEM_CACHE_SIZE=$((${CACHE_SIZES[0]})) \
    TAZER_BB_CACHE=$((${CACHES[1]})) TAZER_BB_CACHE_SIZE=$((${CACHE_SIZES[1]})) \
    TAZER_BOUNDED_FILELOCK_CACHE=${CACHES[2]} TAZER_BOUNDED_FILELOCK_CACHE_SIZE=$((${CACHE_SIZES[2]})) \
    TAZER_PREFETCH=0 LD_PRELOAD=${TAZER_LIB} ${sim_dir}/workloadSim --infile access_pattern.txt --infilesuffix .meta.in --calculatehash true --iorate $x
# /share/apps/gcc/8.1.0/lib64/libasan.so:
    # time gdb --ex run --ex bt --args env  TAZER_REF_TIME=${ref_time} TAZER_SHARED_MEM_CACHE=${CACHES[0]} TAZER_SHARED_MEM_CACHE_SIZE=$((${CACHE_SIZES[0]})) \
    # TAZER_BB_CACHE=${CACHES[1]} TAZER_BB_CACHE_SIZE=$((${CACHE_SIZES[1]})) \
    # TAZER_BOUNDED_FILELOCK_CACHE=${CACHES[2]} TAZER_BOUNDED_FILELOCK_CACHE_SIZE=$((${CACHE_SIZES[2]})) \

    #Compare the two resulting hash sums.
    if [[ $(cmp hash.txt hash2.txt) ]]; then
    #Error details are sent to stderr which is captured in client#.error assuming this is run through launch_tazer_client.sh and run_workflow_tests.sh.
        echo "********** Error: comparison failed **********"
        echo "********** Error: comparison failed **********" 1>&2
        echo "Details:" 1>&2
        echo "Parameters: $inputs --inputFileName=${SERVER_DATA_PATH}/tazer1GB.dat --outputFileName=access_pattern.txt --plot=access_pattern.png" 1>&2
        echo "hash.txt: " $(cat hash.txt) 1>&2
        echo "hash2.txt: " $(cat hash2.txt) 1>&2
        if [ ! CACHE[0] == 0 ]; then 
            echo "Shared Memory Enabled: ${CACHE_SIZES[0]}" 1>&2
        fi
        if [ ! CACHE[1] == 0 ]; then 
            echo "Burst Buffer Enabled: ${CACHE_SIZES[1]}" 1>&2
        fi
        if [ ! CACHE[2] == 0 ]; then 
            echo "Bounded Filelock Enabled: ${CACHE_SIZES[2]}" 1>&2
        fi  
        cp hash.txt $(cat hash.txt)
        exit 1
    fi
} > test_${test_id}.out 2>test_${test_id}.err

exit 0
