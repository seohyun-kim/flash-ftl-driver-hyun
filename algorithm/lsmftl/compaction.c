#include "compaction.h"
#include "run.h"
#include "../../include/sem_lock.h"
#include "../../interface/interface.h"
#include "sst_page_file_stream.h"
#include "io.h"
#include "function_test.h"
#include <algorithm>
extern lsmtree LSM;
typedef struct{
	level *des;
	uint32_t from;
	uint32_t to;
	inter_read_alreq_param *param[COMPACTION_TAGS];
}read_issue_arg;

typedef struct{
	read_issue_arg **arg_set;
	uint32_t set_num;
}read_arg_container;
static compaction_master *_cm;
void *comp_alreq_end_req(algo_req *req);

uint32_t debug_lba=UINT32_MAX;

static inline void compaction_debug_func(uint32_t lba, uint32_t piece_ppa, level *des){
	static int cnt=0;
	if(lba==debug_lba){
		printf("[%d]%u,%u (l,p) -> %u\n",++cnt, lba,piece_ppa, des->idx);
		if(cnt==17){
			printf("break!\n");
		}
	}
}

static inline void compaction_error_check
(key_ptr_pair *kp_set, level *src, level *des, level *res, uint32_t compaction_num){
	uint32_t min_lba=kp_set?
		MIN(kp_set[0].lba, GET_LEV_START_LBA(des)):
		MIN(GET_LEV_START_LBA(des), GET_LEV_START_LBA(src));

	uint32_t max_lba=kp_set?
		MAX(kp_set[LAST_KP_IDX].lba, GET_LEV_END_LBA(des)):
		MAX(GET_LEV_END_LBA(des), GET_LEV_END_LBA(src));

	if(!(GET_LEV_START_LBA(res) <= min_lba
				&& GET_LEV_END_LBA(res)>=max_lba)){
		printf("src\n");
		level_print(src);
		printf("des\n");
		level_print(des);
		printf("res\n");
		level_print(res);
		if(kp_set){
			printf("first_compaction_cnt:%u\n", compaction_num);
		}
		else{
			printf("leveling_compaction_cnt:%u\n", compaction_num);
		}
		EPRINT("range error", true);
	}

	//if(res->idx==2 || res->idx==3){
	//	printf("lev 2,3\n");
	
	//}

}

static inline void tiering_compaction_error_check(level *src, run *r1, run *r2, run *res,
		uint32_t compaction_num){
	uint32_t min_lba, max_lba;
	if(src){
		min_lba=GET_LEV_START_LBA(src);
		max_lba=GET_LEV_END_LBA(src);
	}
	else{
		min_lba=MIN(r1->start_lba, r2->start_lba);	
		max_lba=MAX(r1->start_lba, r2->start_lba);	
	}		
	if(!(res->start_lba==min_lba && res->end_lba==max_lba)){
		if(src){
			printf("tiering_compaction_cnt:%u\n",compaction_num);
		}
		else{
			printf("merge_compaction_cnt:%u\n",compaction_num);	
		}
		EPRINT("range error", true);
	}
}

static void read_sst_job(void *arg, int th_num){
	read_arg_container *thread_arg=(read_arg_container*)arg;
	read_issue_arg **arg_set=thread_arg->arg_set;
	uint32_t *idx_set=(uint32_t*)calloc(thread_arg->set_num, sizeof(uint32_t));
	inter_read_alreq_param ***param_set=
		(inter_read_alreq_param ***)calloc(thread_arg->set_num, sizeof(inter_read_alreq_param**));

	for(uint32_t i=0; i<thread_arg->set_num; i++){
		param_set[i]=arg_set[i]->param;
		idx_set[i]=arg_set[i]->from;
	}

	uint32_t remain_checker=0, target=(1<<(thread_arg->set_num))-1;
	while(!(remain_checker==target)){
		for(uint32_t i=0; i<thread_arg->set_num; i++){
			if(remain_checker&1<<i) continue;
			inter_read_alreq_param *now=param_set[i][idx_set[i]-arg_set[i]->from];
			algo_req *read_req=(algo_req*)malloc(sizeof(algo_req));
			//read_req->ppa=now->target->piece_ppa;
			read_req->type=MAPPINGR;
			read_req->param=(void*)now;
			read_req->end_req=comp_alreq_end_req;
			io_manager_issue_read(now->target->file_addr.map_ppa, 
					now->data, read_req, false);

			if(idx_set[i]==(arg_set[i]->to)){
				remain_checker|=1<<i;
			}
			idx_set[i]++;
		}
	}
	free(idx_set);
	free(param_set);
}

static void read_param_init(read_issue_arg *read_arg){
	inter_read_alreq_param *param;
	for(int i=0; i<read_arg->to-read_arg->from+1; i++){
		param=compaction_get_read_param(_cm);
		param->target=LEVELING_SST_AT_PTR(read_arg->des, read_arg->from+i);
		param->data=inf_get_valueset(NULL, FS_MALLOC_R, PAGESIZE);
		fdriver_lock_init(&param->done_lock, 0);
		read_arg->param[i]=param;
	}
}

bool read_done_check(inter_read_alreq_param *param, bool check_page_sst){
	if(check_page_sst){
		param->target->data=param->data->value;
	}
	fdriver_lock(&param->done_lock);
	return true;
}

bool file_done(inter_read_alreq_param *param){
	param->target->data=NULL;
	inf_free_valueset(param->data, FS_MALLOC_R);
	fdriver_destroy(&param->done_lock);
	compaction_free_read_param(_cm, param);
	return true;
}

static void stream_sorting(level *des, uint32_t stream_num, sst_pf_out_stream **os_set, 
		sst_pf_in_stream *is, void (*write_function)(sst_pf_in_stream* is,  level*), bool all_empty_stop){
	bool one_empty=false;
	if(stream_num >= 32){
		EPRINT("too many stream!!", true);
	}
	
	uint32_t all_empty_check=0;
	uint32_t sorting_idx=0;
	while(!((all_empty_stop && all_empty_check==((1<<stream_num)-1)) || (!all_empty_stop && one_empty))){
		key_ptr_pair target_pair;
		target_pair.lba=UINT32_MAX;
		uint32_t target_idx=UINT32_MAX;
		for(uint32_t i=0; i<stream_num; i++){
			if(all_empty_check & 1<<i) continue;
			key_ptr_pair now=sst_pos_pick(os_set[i]);

			if(target_idx==UINT32_MAX){
				 target_pair=now;
				 target_idx=i;
			}
			else{
				if(target_pair.lba > now.lba){
					target_pair=now;
					target_idx=i;
				}
				else if(target_pair.lba!=UINT32_MAX && target_pair.lba==now.lba){
					invalidate_piece_ppa(LSM.pm->bm,now.piece_ppa);
					sst_pos_pop(os_set[i]);
					continue;
				}
				else{
					continue;
				}
			}
		}
		if(target_pair.lba!=UINT32_MAX){
			sorting_idx++;
			compaction_debug_func(target_pair.lba, target_pair.piece_ppa, des);
			if(sst_pis_insert(is, target_pair)){
				if(write_function){
					write_function(is, des);
				}
			}

			sst_pos_pop(os_set[target_idx]);
		}
		if(sst_pos_is_empty(os_set[target_idx])){
			one_empty=true;
			all_empty_check|=1<<target_idx;
		}
	}

	if(all_empty_stop && sst_pis_remain_data(is)){
		if(write_function){
			write_function(is,des);
		}
	}

}

static void write_sst_file(sst_pf_in_stream *is, level *des){ //for page file
	sst_file *sptr;
	value_set *vs=sst_pis_get_result(is, &sptr);
	sptr->file_addr.map_ppa=page_manager_get_new_ppa(LSM.pm,true);
	
	algo_req *write_req=(algo_req*)malloc(sizeof(algo_req));
	write_req->type=MAPPINGW;
	write_req->param=(void*)vs;
	write_req->end_req=comp_alreq_end_req;

	io_manager_issue_internal_write(sptr->file_addr.map_ppa, vs, write_req, false);
	sptr->data=NULL;
	level_append_sstfile(des, sptr);
	sst_free(sptr);
}


static sst_file *key_ptr_to_sst_file(key_ptr_pair *kp_set, bool should_flush){
	uint32_t ppa=UINT32_MAX;
	if(should_flush){
		value_set *vs=inf_get_valueset((char*)kp_set, FS_MALLOC_W, PAGESIZE);
		ppa=page_manager_get_new_ppa(LSM.pm, true);
		algo_req *write_req=(algo_req*)malloc(sizeof(algo_req));
		write_req->type=MAPPINGW;
		write_req->param=(void*)vs;
		write_req->end_req=comp_alreq_end_req;
		io_manager_issue_internal_write(ppa, vs, write_req, false);
	}
	sst_file *sstfile=sst_pf_init(ppa, kp_set[0].lba, kp_set[LAST_KP_IDX].lba);
	return sstfile;
}

static void trivial_move(key_ptr_pair *kp_set,level *up, level *down, level *des){
	uint32_t ridx, sidx;
	LSM.monitor.trivial_move_cnt++;
	run *rptr; sst_file *sptr; 
	if(kp_set){ // L0
		sst_file *file=key_ptr_to_sst_file(kp_set, true);
		file->_read_helper=read_helper_kpset_to_rh(LSM.param.leveling_rhp, kp_set);
		if(down->now_sst_num==0){
			level_append_sstfile(des,file);
		}
		else{
			if(LAST_RUN_PTR(down)->end_lba < file->start_lba){
				for_each_sst_level(down, rptr, ridx, sptr, sidx){
					level_append_sstfile(des, sptr);
				}
			}
			level_append_sstfile(des,file);
			if(FIRST_RUN_PTR(down)->start_lba > file->end_lba){
				for_each_sst_level(down, rptr, ridx, sptr, sidx){
					level_append_sstfile(des, sptr);
				}
			}
		}
		sst_free(file);
	}
	else{ //L1~LN-1
		if(down->now_sst_num==0){
			for_each_sst_level(up, rptr, ridx, sptr, sidx){
				level_append_sstfile(des, sptr);
			}
		}
		else{
			if(LAST_RUN_PTR(down)->end_lba < FIRST_RUN_PTR(up)->start_lba){
				for_each_sst_level(down, rptr, ridx, sptr, sidx){
					level_append_sstfile(des, sptr);
				}
			}

			for_each_sst_level(up, rptr, ridx, sptr, sidx){
				level_append_sstfile(des, sptr);
			}

			if(FIRST_RUN_PTR(down)->start_lba > LAST_RUN_PTR(up)->end_lba){
				for_each_sst_level(down, rptr, ridx, sptr, sidx){
					level_append_sstfile(des, sptr);
				}
			}

		}
	}
	free(kp_set);
}

static void compaction_move_unoverlapped_sst
(key_ptr_pair* kp_set, level*up, level *down, level *des, uint32_t* start_idx){
	bool is_close_target=false;
	uint32_t target_start_lba=kp_set?kp_set[0].lba:GET_LEV_START_LBA(up);

	sst_file *start_sst_file=level_retrieve_sst(down, target_start_lba);
	if(!start_sst_file){
		is_close_target=true;
		start_sst_file=level_retrieve_close_sst(down, target_start_lba);
		if(!start_sst_file){
			*start_idx=0;
			return;
		}
	}

	uint32_t _start_idx=GET_SST_IDX(down, start_sst_file);
	for(uint32_t i=0; i<_start_idx; i++){
		level_append_sstfile(des, LEVELING_SST_AT_PTR(down,i));
	}
	if(is_close_target){
		level_append_sstfile(des, LEVELING_SST_AT_PTR(down, _start_idx));
	}

	*start_idx=_start_idx;
}

level* compaction_first_leveling(compaction_master *cm, key_ptr_pair *kp_set, level *des){
	static int debug_cnt_flag=0;
	_cm=cm;
	level *res=level_init(des->max_sst_num, des->run_num, des->istier, des->idx);

	if(!level_check_overlap_keyrange(kp_set[0].lba, kp_set[LAST_KP_IDX].lba, des)){

		trivial_move(kp_set,NULL,  des, res);
		//level_print(res);
		return res;
	}
	LSM.monitor.compaction_cnt[0]++;
	/*each round read/write 128 data*/
	/*we need to have rate limiter!!*/
	read_issue_arg read_arg;
	read_arg.des=des;
	read_arg_container thread_arg;
	thread_arg.arg_set=(read_issue_arg**)malloc(sizeof(read_issue_arg));
	thread_arg.arg_set[0]=&read_arg;
	thread_arg.set_num=1;

	sst_pf_out_stream *os_set[2]={NULL,};
	sst_pf_out_stream *os;
	sst_pf_in_stream *is=sst_pis_init(true, LSM.param.leveling_rhp);

	uint32_t start_idx=0;
	compaction_move_unoverlapped_sst(kp_set, NULL, des, res, &start_idx);
	uint32_t total_num=des->now_sst_num-start_idx;
	uint32_t level_compaction_tags=COMPACTION_TAGS/2;
	uint32_t round=total_num/level_compaction_tags+(total_num%level_compaction_tags?1:0);
	for(uint32_t i=0; i<round; i++){
		/*set read_arg*/
		read_arg.from=start_idx+i*level_compaction_tags;
		if(i!=round-1){
			read_arg.to=start_idx+(i+1)*level_compaction_tags-1;
		}
		else{
			read_arg.to=des->now_sst_num-1;	
		}
		read_param_init(&read_arg);

		if(i==0){
			os=sst_pos_init(LEVELING_SST_AT_PTR(des, read_arg.from), read_arg.param, 
					read_arg.to-read_arg.from+1, read_done_check, file_done);
			os_set[0]=sst_pos_init_kp(kp_set);
			os_set[1]=os;
		}
		else{
			sst_pos_add(&os[1], LEVELING_SST_AT_PTR(des, read_arg.from), read_arg.param,
					read_arg.to-read_arg.from+1);
		}
		/*send read I/O*/
		thpool_add_work(cm->issue_worker, read_sst_job, (void*)&thread_arg);

		stream_sorting(res, 2, os_set, is, write_sst_file, i==round-1);

	}
	
	thpool_wait(cm->issue_worker);
	sst_pos_free(os_set[0]);
	sst_pos_free(os_set[1]);

	sst_pis_free(is);

	//level_print(res);
	compaction_error_check(kp_set, NULL, des, res, debug_cnt_flag++);

	free(kp_set);
	free(thread_arg.arg_set);
	_cm=NULL;
	return res;
}

level* compaction_leveling(compaction_master *cm, level *src, level *des){
	static int debug_cnt_flag=0;
	if(debug_cnt_flag==28){

	}
	_cm=cm;
	level *res=level_init(des->max_sst_num, des->run_num, des->istier, des->idx);
	if(!level_check_overlap(src, des)){
		trivial_move(NULL,src, des, res);
		return res;
	}
	LSM.monitor.compaction_cnt[des->idx]++;

	read_issue_arg read_arg1, read_arg2;
	read_arg1.des=src;
	read_arg2.des=des;

	read_arg_container thread_arg;
	thread_arg.arg_set=(read_issue_arg**)malloc(sizeof(read_issue_arg*)*2);
	thread_arg.arg_set[0]=&read_arg1;
	thread_arg.arg_set[1]=&read_arg2;
	thread_arg.set_num=2;

	sst_pf_out_stream *os_set[2];
	sst_pf_in_stream *is=sst_pis_init(true, LSM.param.leveling_rhp);
	
	uint32_t des_start_idx=0;
	uint32_t des_end_idx=0;
	compaction_move_unoverlapped_sst(NULL,src, des, res, &des_start_idx);

	run *rptr; sst_file *sptr;
	uint32_t ridx, sidx;
	uint32_t lower_last_idx=des_start_idx;
	bool isstart=true;
	uint32_t stream_cnt=0;
	for_each_sst_level(src, rptr, ridx, sptr, sidx){
		/*set read_arg start*/
		read_arg1.from=sidx;
		read_arg2.from=des_start_idx;
		uint32_t round=sidx;
		uint32_t now_remain_tags=compaction_read_param_remain_num(cm);
		do{
			sst_file *down_target=level_retrieve_sst(des, sptr->end_lba);
			if(!down_target){
				down_target=level_retrieve_close_sst(des, sptr->end_lba);
			}
			if((round-read_arg1.from+1)+(GET_SST_IDX(des,down_target)-read_arg2.from+1) < now_remain_tags){
				read_arg1.to=round;
				read_arg2.to=GET_SST_IDX(des, down_target);
				des_end_idx=read_arg2.to;

				sidx++;
				if(!(sidx<src->now_sst_num)){
					sidx--;
					break;
				}
				sptr=LEVELING_SST_AT_PTR(src, sidx);
				round++;
			}
			else{
				break;
			}
		}while(1);
		
		
		read_param_init(&read_arg1);
		read_param_init(&read_arg2);


		if(isstart){
			os_set[0]=sst_pos_init(LEVELING_SST_AT_PTR(src, read_arg1.from), read_arg1.param, 
					read_arg1.to-read_arg1.from+1, read_done_check, file_done);
			os_set[1]=sst_pos_init(LEVELING_SST_AT_PTR(des, read_arg2.from), read_arg2.param, 
					read_arg2.to-read_arg2.from+1, read_done_check, file_done);
		}
		else{
			sst_pos_add(os_set[0], LEVELING_SST_AT_PTR(src, read_arg1.from), read_arg1.param,
					read_arg1.to-read_arg1.from+1);
			sst_pos_add(os_set[1], LEVELING_SST_AT_PTR(des, read_arg2.from), read_arg2.param,
					read_arg2.to-read_arg2.from+1);
		}
		/*send read I/O*/
		thpool_add_work(cm->issue_worker, read_sst_job, (void*)&thread_arg);

		stream_sorting(res, 2, os_set, is, write_sst_file, sidx==src->now_sst_num-1);


		isstart=false;
		des_start_idx=des_end_idx+1;
		sidx=read_arg1.to;
		stream_cnt++;
	}

	for(uint32_t i=des_start_idx; i<des->now_sst_num; i++){
		level_append_sstfile(res, LEVELING_SST_AT_PTR(des, i));
	}

	thpool_wait(cm->issue_worker);
	sst_pos_free(os_set[0]);
	sst_pos_free(os_set[1]);

	sst_pis_free(is);

	/*level error check*/
	
	compaction_error_check(NULL, src, des, res, debug_cnt_flag++);
	free(thread_arg.arg_set);


	_cm=NULL;
	return res;

}

static int issue_read_kv_for_bos(sst_bf_out_stream *bos, sst_pf_out_stream *pos, 
		uint32_t target_num, bool round_final){
	key_value_wrapper *read_target;
	uint32_t res=0;
	for(uint32_t i=0; i<target_num  && !sst_pos_is_empty(pos); i++){
		key_ptr_pair target_pair=sst_pos_pick(pos);
		if(target_pair.lba==UINT32_MAX) continue;
		key_value_wrapper *kv_wrapper=(key_value_wrapper*)calloc(1,sizeof(key_value_wrapper));

		kv_wrapper->piece_ppa=target_pair.piece_ppa;
		kv_wrapper->kv_ptr.lba=target_pair.lba;

		if((read_target=sst_bos_add(bos, kv_wrapper, _cm))){
			if(!read_target->param){
				EPRINT("can't be",true);
			}
			algo_req *read_req=(algo_req*)malloc(sizeof(algo_req));
			read_req->type=COMPACTIONDATAR;
			read_req->param=(void*)read_target;
			read_req->end_req=comp_alreq_end_req;
			io_manager_issue_read(PIECETOPPA(read_target->piece_ppa),
					read_target->param->data, read_req, false);
		}

		sst_pos_pop(pos);
		res=i;
	}

	if(sst_pos_is_empty(pos) && round_final){
		if((read_target=sst_bos_get_pending(bos, _cm))){
			algo_req *read_req=(algo_req*)malloc(sizeof(algo_req));
			read_req->type=DATAR;
			read_req->param=(void*)read_target;
			read_req->end_req=comp_alreq_end_req;		
			io_manager_issue_read(PIECETOPPA(read_target->piece_ppa),
					read_target->param->data, read_req, false);
		}
	}
	return res;
}


static sst_file *bis_to_sst_file(sst_bf_in_stream *bis){
	if(bis->map_data->size()==0) return NULL;
	sst_file *res=sst_init_empty(BLOCK_FILE);
	uint32_t map_num=0;
	uint32_t ppa;
	map_range *mr_set=(map_range*)malloc(sizeof(map_range) * 
			bis->map_data->size());
	uint32_t mr_idx=0;
	while(bis->map_data->size()){
		value_set *data=bis->map_data->front();
		algo_req *write_req=(algo_req*)malloc(sizeof(algo_req));
		write_req->type=DATAW;
		write_req->param=(void*)data;
		write_req->end_req=comp_alreq_end_req;

		map_num++;

		ppa=page_manager_get_new_ppa(LSM.pm, false);
		if(bis->start_piece_ppa/2/_PPS!=ppa/_PPS){
			EPRINT("map data should same sgement", true);
		}

		mr_set[mr_idx].start_lba=((key_ptr_pair*)data->value)[0].lba;
		mr_set[mr_idx].end_lba=kp_get_end_lba(data->value);
		mr_set[mr_idx].ppa=ppa;

		io_manager_issue_write(ppa, data, write_req, false);

		mr_idx++;
		bis->map_data->pop();
	}

	sst_set_file_map(res,mr_idx, mr_set);

	res->file_addr.piece_ppa=bis->start_piece_ppa;
	res->end_ppa=ppa;
	res->map_num=map_num;
	res->start_lba=bis->start_lba;
	res->end_lba=bis->end_lba;
	res->_read_helper=bis->rh;
	bis->rh=NULL;
	return res;
}

static void issue_write_kv_for_bis(sst_bf_in_stream **bis, sst_bf_out_stream *bos, run *new_run,
		int32_t entry_num, bool final){
	int32_t inserted_entry_num=0;
	while(!sst_bos_is_empty(bos)){
		key_value_wrapper *target=NULL;
		if(!final && !(target=sst_bos_pick(bos, entry_num-inserted_entry_num <=L2PGAP))){
			break;
		}
		else if(final){
			target=sst_bos_pick(bos, entry_num-inserted_entry_num<=L2PGAP);
			if(!target){
				target=sst_bos_get_pending(bos, _cm);
			}
		}

		if(target){
			invalidate_piece_ppa((*bis)->pm->bm, target->piece_ppa);
		}


		compaction_debug_func(target->kv_ptr.lba, target->piece_ppa,
				LSM.disk[LSM.param.LEVELN-1]);

		if((target && sst_bis_insert(*bis, target)) ||
				(final && sst_bos_kv_q_size(bos)==1)){
			value_set *result=sst_bis_get_result(*bis, final);
			algo_req *write_req=(algo_req*)malloc(sizeof(algo_req));
			write_req->type=COMPACTIONDATAW;
			write_req->param=(void*)result;
			write_req->end_req=comp_alreq_end_req;
			io_manager_issue_write(result->ppa, result, write_req, false);
		}

		sst_bos_pop(bos, _cm);
	//	if(!final & sst_bis_ppa_empty(*bis)){
		if(sst_bis_ppa_empty(*bis)){
			sst_file *sptr=bis_to_sst_file(*bis);
			run_append_sstfile(new_run, sptr);
			sst_free(sptr);
			sst_bis_free(*bis);
			uint32_t start_piece_ppa=page_manager_pick_new_ppa(LSM.pm, false)*L2PGAP;
			uint32_t piece_ppa_length=page_manager_get_remain_page(LSM.pm, false)*L2PGAP;
			read_helper_param temp_rhp=LSM.param.tiering_rhp;
			temp_rhp.member_num=piece_ppa_length;
			*bis=sst_bis_init(start_piece_ppa, piece_ppa_length, LSM.pm, true, temp_rhp);
		}
		inserted_entry_num++;
	}
}

level* compaction_tiering(compaction_master *cm, level *src, level *des){ /*move to last level*/
	_cm=cm;
	level *res=level_init(des->max_sst_num, des->max_run_num, des->istier, des->idx);

	run *rptr; uint32_t ridx;
	for_each_run(des, rptr, ridx){
		level_deep_append_run(res, rptr);
	}
	LSM.monitor.compaction_cnt[des->idx]++;

	run *new_run=run_init(des->max_sst_num/des->max_run_num, UINT32_MAX, 0);

	read_issue_arg read_arg;
	read_arg.des=src;
	read_arg_container thread_arg;
	thread_arg.arg_set=(read_issue_arg**)malloc(sizeof(read_issue_arg));
	thread_arg.arg_set[0]=&read_arg;
	thread_arg.set_num=1;

	sst_pf_out_stream *pos;
	sst_bf_out_stream *bos=sst_bos_init(read_done_check);
	uint32_t start_piece_ppa=page_manager_pick_new_ppa(LSM.pm, false)*L2PGAP;
	uint32_t piece_ppa_length=page_manager_get_remain_page(LSM.pm, false)*L2PGAP;
	read_helper_param temp_rhp=LSM.param.tiering_rhp;
	temp_rhp.member_num=piece_ppa_length;
	sst_bf_in_stream *bis=sst_bis_init(start_piece_ppa, piece_ppa_length, LSM.pm,true, temp_rhp);
	uint32_t start_idx=0;
	uint32_t total_num=src->now_sst_num;
	uint32_t tier_compaction_tags=COMPACTION_TAGS/2;
	uint32_t round=total_num/tier_compaction_tags+(total_num%tier_compaction_tags?1:0);
	for(uint32_t i=0; i<round; i++){
		if(i==round-1){
			printf("break!\n");
		}
		read_arg.from=start_idx+i*tier_compaction_tags;
		if(i!=round-1){
			read_arg.to=start_idx+(i+1)*tier_compaction_tags-1;
		}
		else{
			read_arg.to=src->now_sst_num-1;	
		}
		read_param_init(&read_arg);

		if(i==0){
			pos=sst_pos_init(LEVELING_SST_AT_PTR(src, read_arg.from), read_arg.param, 
					read_arg.to-read_arg.from+1, read_done_check, file_done);
		}
		else{
			sst_pos_add(pos, LEVELING_SST_AT_PTR(src, read_arg.from), read_arg.param,
					read_arg.to-read_arg.from+1);
		}
		read_sst_job((void*)&thread_arg,-1);
		
		uint32_t round2_tier_compaction_tags, picked_kv_num;
		key_ptr_pair temp_kv_pair;
		uint32_t j=0;

		do{ 
			round2_tier_compaction_tags=cm->read_param_queue->size();
			picked_kv_num=issue_read_kv_for_bos(bos, pos, round2_tier_compaction_tags, i==round-1);
			round2_tier_compaction_tags=MIN(round2_tier_compaction_tags, picked_kv_num);
			issue_write_kv_for_bis(&bis, bos, new_run, round2_tier_compaction_tags, i==round-1 && sst_pos_is_empty(pos));
			j++;
		}
		while((temp_kv_pair=sst_pos_pick(pos)).lba!=UINT32_MAX);
	}

	//finishing bis
	sst_file *last_file;
	if((last_file=bis_to_sst_file(bis))){
		run_append_sstfile(new_run, last_file);
		sst_free(last_file);
	}
	
	sst_pos_free(pos);
	sst_bis_free(bis);
	sst_bos_free(bos, _cm);
	
	tiering_compaction_error_check(src, NULL, NULL, new_run, LSM.monitor.compaction_cnt[des->idx]);

	level_append_run(res, new_run);
	run_free(new_run);
	free(thread_arg.arg_set);
	/*finish logic*/
	return res;
}

level* compaction_merge(compaction_master *cm, run *r1, run* r2, uint8_t version_idx){
	EPRINT("Not implemented", true);
	return NULL;
}

void *comp_alreq_end_req(algo_req *req){
	inter_read_alreq_param *r_param;
	value_set *vs;
	key_value_wrapper *kv_wrapper;
	switch(req->type){
		case MAPPINGW:
			vs=(value_set*)req->param;
			inf_free_valueset(vs, FS_MALLOC_W);
			break;
		case MAPPINGR:
			r_param=(inter_read_alreq_param*)req->param;
			fdriver_unlock(&r_param->done_lock);
			break;
		case COMPACTIONDATAR:
		case DATAR:
			kv_wrapper=(key_value_wrapper*)req->param;
			fdriver_unlock(&kv_wrapper->param->done_lock);
			break;
		case COMPACTIONDATAW:
		case DATAW:
			vs=(value_set*)req->param;
			inf_free_valueset(vs, FS_MALLOC_W);
			break;
	}
	free(req);
	return NULL;
}