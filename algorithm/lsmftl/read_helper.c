#include "read_helper.h"
#include "helper_algorithm/bf_set.h"
#include "helper_algorithm/guard_bf_set.h"
#include "helper_algorithm/plr_helper.h"
#include "key_value_pair.h"
extern uint32_t debug_lba;

void read_helper_prepare(float target_fpr, uint32_t member, uint32_t type){
	bf_set_prepare(target_fpr, member, type);
	gbf_set_prepare(target_fpr, member, type);
}

read_helper *read_helper_init(read_helper_param rhp){
	if(rhp.type==HELPER_NONE) return NULL;
	read_helper *res=(read_helper*)malloc(sizeof(read_helper));
	res->type=rhp.type;
	switch(rhp.type){
		case HELPER_BF_PTR:
			res->body=(void*)bf_set_init(rhp.target_prob, rhp.member_num, BLOOM_PTR_PAIR);
			break;
		case HELPER_BF_ONLY:
			res->body=(void*)bf_set_init(rhp.target_prob, rhp.member_num, BLOOM_ONLY);
			break;
		case HELPER_BF_PTR_GUARD:
			res->body=(void*)gbf_set_init(rhp.target_prob, rhp.member_num, BLOOM_PTR_PAIR);
			break;
		case HELPER_BF_ONLY_GUARD:
			res->body=(void*)gbf_set_init(rhp.target_prob, rhp.member_num, BLOOM_ONLY);
			break;
		case HELPER_PLR:
			res->body=(void*)plr_init(rhp.slop_bit, rhp.range, rhp.member_num);
			break;
		default:
			EPRINT("not collect type",true);
			break;
	}
		
	return res;
}

read_helper *read_helper_kpset_to_rh(read_helper_param rhp, key_ptr_pair *kp_set){
	if(rhp.type==HELPER_NONE) return NULL;

	read_helper *res=(read_helper*)malloc(sizeof(read_helper));
	res->type=rhp.type;

	uint32_t i=0;
	switch(rhp.type){
		case HELPER_BF_PTR:
			res->body=(void*)bf_set_init(rhp.target_prob, rhp.member_num, BLOOM_PTR_PAIR);
			for(;i<KP_IN_PAGE && kp_set[i].lba!=UINT32_MAX; i++){
				bf_set_insert((bf_set*)res->body, kp_set[i].lba, kp_set[i].piece_ppa);
			}
			break;
		case HELPER_BF_PTR_GUARD:
			res->body=(void*)gbf_set_init(rhp.target_prob, rhp.member_num, BLOOM_PTR_PAIR);
			for(;i<KP_IN_PAGE && kp_set[i].lba!=UINT32_MAX; i++){
				gbf_set_insert((guard_bf_set*)res->body, kp_set[i].lba, kp_set[i].piece_ppa);
			}
			break;
		case HELPER_PLR:
		case HELPER_BF_ONLY_GUARD:
		case HELPER_BF_ONLY:
			EPRINT("cannot",true);
			break;
		default:
			EPRINT("not collect type",true);
			break;
	}
	read_helper_insert_done(res);
	return res;
}
uint32_t read_helper_stream_insert(read_helper *rh, uint32_t lba, uint32_t piece_ppa){
	if(!rh) return 1;
	if(lba==debug_lba){
		EPRINT("debug point", false);
	}
	switch(rh->type){
		case HELPER_BF_PTR:
		case HELPER_BF_ONLY:
			bf_set_insert((bf_set*)rh->body,lba,piece_ppa);
			break;
		case HELPER_BF_ONLY_GUARD:
		case HELPER_BF_PTR_GUARD:
			gbf_set_insert((guard_bf_set*)rh->body, lba, piece_ppa);
			break;
		case HELPER_PLR:
			plr_insert((plr_helper*)rh->body, lba, piece_ppa/L2PGAP);
			break;
		default:
			EPRINT("not collect type",true);
			break;
	}
	return 1;
}

uint32_t read_helper_memory_usage(read_helper *rh){
	if(!rh) return 0;
	
	switch(rh->type){
		case HELPER_BF_PTR:
		case HELPER_BF_ONLY:
			return ((bf_set*)rh->body)->memory_usage_bit;
		case HELPER_BF_PTR_GUARD:
		case HELPER_BF_ONLY_GUARD:
			return gbf_get_memory_usage_bit((guard_bf_set*)rh->body);
		case HELPER_PLR:
			return plr_memory_usage((plr_helper*)rh->body);
		default:
			EPRINT("not collect type",true);
			break;
	}

	return UINT32_MAX;
}

static inline uint32_t adjust_piece_ppa(uint8_t type, uint32_t hp_result_piece_ppa, uint32_t start_addr_piece_ppa, sst_file *sptr){
	uint32_t res=0;
	uint32_t i;
	map_range *mr;
	switch(type){
		case HELPER_BF_ONLY_GUARD:
		case HELPER_BF_ONLY:
			if(sptr->sequential_file){
				mr=sptr->block_file_map;
				for(i=0; i<sptr->map_num; i++){
					if(mr[i].ppa <= (start_addr_piece_ppa+hp_result_piece_ppa)/L2PGAP){
						hp_result_piece_ppa+=L2PGAP;
					}
					else break;
				}
				res=hp_result_piece_ppa+start_addr_piece_ppa;
			}
			else{
				res=hp_result_piece_ppa+start_addr_piece_ppa;
			}
			break;
		default:
			EPRINT("error", true);
			break;
	}
	return res;
}

bool read_helper_check(read_helper *rh, uint32_t lba, uint32_t *piece_ppa_result, 
		sst_file *sptr, uint32_t *idx){
	if(!rh){
		EPRINT("no rh", true);
		return true;
	}

	if((*idx)==UINT32_MAX) 
		return false;
	uint32_t ppa;
	switch(rh->type){
		case HELPER_BF_PTR:
			*piece_ppa_result=bf_set_get_piece_ppa((bf_set*)rh->body, 
					idx, lba);
			if(*piece_ppa_result==UINT32_MAX){
				(*idx)=UINT32_MAX;
				return false;
			}
			else{
				(*idx)--;
				return true;
			}
		case HELPER_BF_ONLY:
			*piece_ppa_result=bf_set_get_piece_ppa((bf_set*)rh->body, 
					idx, lba);
			if(*piece_ppa_result==UINT32_MAX){
				(*idx)=UINT32_MAX;
				return false;
			}
			else{
				if(sptr->type!=BLOCK_FILE){
					EPRINT("read_helper miss match", true);
				}
				*piece_ppa_result=adjust_piece_ppa(rh->type, *piece_ppa_result, sptr->file_addr.piece_ppa, sptr);
				(*idx)--;
				return true;
			}
		case HELPER_BF_PTR_GUARD:
			*piece_ppa_result=gbf_set_get_piece_ppa((guard_bf_set*)rh->body,
					idx, lba);
			if(*piece_ppa_result==UINT32_MAX){
				(*idx)=UINT32_MAX;
				return false;
			}
			else{
				(*idx)--;
				return true;
			}
		case HELPER_BF_ONLY_GUARD:
			if(lba==debug_lba){
				printf("debug_break!\n");
			}
			*piece_ppa_result=gbf_set_get_piece_ppa((guard_bf_set*)rh->body,
					idx, lba);
			if(*piece_ppa_result==UINT32_MAX){
				(*idx)=UINT32_MAX;
				return false;
			}
			else{
				if(sptr->type!=BLOCK_FILE){
					EPRINT("read_helper miss match", true);
				}
				*piece_ppa_result=adjust_piece_ppa(rh->type, *piece_ppa_result, sptr->file_addr.piece_ppa, sptr);
				(*idx)--;
				return true;
			}
		case HELPER_PLR:
			if(lba==debug_lba){
				printf("debug_break!\n");
			}
			if((*idx)==PLR_SECOND_ROUND){
				(*idx)=UINT32_MAX;
				return false;
			}
			ppa=plr_get_ppa((plr_helper*)rh->body, lba, (*piece_ppa_result)/L2PGAP, idx);
			*piece_ppa_result=ppa*L2PGAP;
			return true;
		default:
			EPRINT("not collect type",true);
			break;
	}
	return true;
}

void read_helper_print(read_helper *rh){
	EPRINT("not implemented!", true);
}

void read_helper_free(read_helper *rh){
	if(!rh) return;
	switch(rh->type){
		case HELPER_BF_ONLY:
		case HELPER_BF_PTR:
			bf_set_free((bf_set*)rh->body);
			break;
		case HELPER_BF_PTR_GUARD:
		case HELPER_BF_ONLY_GUARD:
			gbf_set_free((guard_bf_set*)rh->body);
			break;
		case HELPER_PLR:
			plr_free((plr_helper*)rh->body);
			break;
		default:
			EPRINT("not collect type",true);
			break;
	}
	free(rh);
}	

read_helper* read_helper_copy(read_helper *src){
	if(!src) return NULL;
	read_helper *res=(read_helper*)malloc(sizeof(read_helper));
	*res=*src;
	switch(src->type){
		case HELPER_BF_PTR:
		case HELPER_BF_ONLY:
			res->body=(void*)bf_set_copy( (bf_set*)src->body);
			//bf_set_free((bf_set*)rh->body);
			break;
		case HELPER_BF_PTR_GUARD:
		case HELPER_BF_ONLY_GUARD:
			res->body=(void*)gbf_set_copy((guard_bf_set*)src->body);
			break;
		case HELPER_PLR:
			res->body=(void*)plr_copy((plr_helper*)src->body);
			break;
		default:
			EPRINT("not collect type",true);
			break;
	}
	return res;
}

void read_helper_move(read_helper *des, read_helper *src){
	if(!src) return ;
	void *temp_body=des->body;
	*des=*src;
	des->body=temp_body;
	switch(des->type){
		case HELPER_BF_PTR:
		case HELPER_BF_ONLY:
			bf_set_move((bf_set*)des->body, (bf_set*)src->body);
			//bf_set_free((bf_set*)rh->body);
			break;
		case HELPER_BF_PTR_GUARD:
		case HELPER_BF_ONLY_GUARD:
			gbf_set_move((guard_bf_set*)des->body, (guard_bf_set*)src->body);
			break;
		case HELPER_PLR:
			plr_move((plr_helper*)des->body, (plr_helper*)src->body);
			break;
		default:
			EPRINT("not collect type",true);
			break;
	}
}

bool read_helper_last(read_helper *rh, uint32_t idx){
	if(!rh) return true;
	switch(rh->type){
		case HELPER_BF_ONLY:
		case HELPER_BF_PTR:
		case HELPER_BF_ONLY_GUARD:
		case HELPER_BF_PTR_GUARD:
		case HELPER_PLR:
			if((int32_t)idx==-1) return true;
			break;
		default:
			EPRINT("not collect type",true);
			break;
	}
	return false;
}

uint32_t read_helper_idx_init(read_helper *rh, uint32_t lba){ 
	if(!rh) return UINT32_MAX;
	switch(rh->type){
		case HELPER_BF_ONLY:
		case HELPER_BF_PTR:
			return ((bf_set*)rh->body)->now-1; //number to idx
		case HELPER_BF_ONLY_GUARD:
		case HELPER_BF_PTR_GUARD:
			return gbf_get_start_idx((guard_bf_set*)rh->body, lba);
		case HELPER_PLR:
			return PLR_NORMAL_PPA;
		default:
			EPRINT("not collect type",true);
			break;
	}
	return UINT32_MAX;
}

static inline int PLR_checking_oob(uint32_t lba, uint32_t piece_ppa, uint32_t *lba_set){
	for(uint32_t i=0; i<L2PGAP; i++){
		if(i==0 && lba < lba_set[i]){
			return -1;
		}
		if(lba==lba_set[i]) return i;
	}
	return L2PGAP;
}

bool read_helper_data_checking(read_helper *rh, page_manager* pm, uint32_t piece_ppa, 
		uint32_t lba, uint32_t *rh_idx, uint32_t *offset){
	if(lba==debug_lba){
		EPRINT("debug point", false);
	}
	switch(rh->type){
		case HELPER_BF_ONLY:
		case HELPER_BF_PTR:
		case HELPER_BF_ONLY_GUARD:
		case HELPER_BF_PTR_GUARD:
			if(page_manager_oob_lba_checker(pm, piece_ppa, lba, offset)){
				return true;
			}
			break;
		case HELPER_PLR:
			(*offset)=PLR_checking_oob(lba, piece_ppa, 
					(uint32_t*)pm->bm->get_oob(pm->bm, PIECETOPPA(piece_ppa)));
			switch((int32_t)(*offset)){
				case -1:
					if((*rh_idx)==PLR_NORMAL_PPA) (*rh_idx)=PLR_FRONT_PPA;
					else (*rh_idx)=PLR_SECOND_ROUND;
					return false;
				case L2PGAP:
					if((*rh_idx)==PLR_NORMAL_PPA) (*rh_idx)=PLR_BEHIND_PPA;
					else (*rh_idx)=PLR_SECOND_ROUND;
					return false;			
				default:
					return true;
			}
			break;
		default:
			EPRINT("not collect type",true);
			break;
	}
	return false;
}

void read_helper_insert_done(read_helper *rh){
	switch(rh->type){
		case HELPER_BF_ONLY:
		case HELPER_BF_PTR:
		case HELPER_BF_ONLY_GUARD:
		case HELPER_BF_PTR_GUARD:
			return;
		case HELPER_PLR:
			plr_insert_done((plr_helper*)rh->body);
			break;
	}
}
