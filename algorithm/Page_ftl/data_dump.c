#include "../../include/settings.h"
#include "../../include/container.h"
#include "page.h"
#include "map.h"
extern algorithm page_ftl;
extern align_buffer a_buffer;

uint32_t page_dump(FILE *fp){
	/*write data in align_buffer*/
	printf("now position :%lu\n", ftell(fp));
	fwrite(&a_buffer.idx, sizeof(uint8_t), 1, fp);
	for(uint32_t i=0; i<a_buffer.idx; i++){
		fwrite(a_buffer.value[i]->value, sizeof(char), PAGESIZE, fp);
	}
	fwrite(a_buffer.key, sizeof(uint32_t), a_buffer.idx, fp);

	pm_body *p=(pm_body*)page_ftl.algo_body;
	uint32_t temp_seg_idx=UINT32_MAX;
	/*write active segment and reserve*/
	if(p->active){
		fwrite(&p->active->seg_idx, sizeof(uint32_t),1,fp);
	}
	else{
		fwrite(&temp_seg_idx, sizeof(uint32_t),1,fp);
	}

	if(p->reserve){
		fwrite(&p->reserve->seg_idx, sizeof(uint32_t),1,fp);
	}
	else{
		fwrite(&temp_seg_idx, sizeof(uint32_t),1,fp);
	}

	/*write mapping data*/
	fwrite(p->mapping, sizeof(uint32_t), _NOP*L2PGAP, fp);

	return 1;
}

extern void page_create_body(lower_info *li, blockmanager *bm, algorithm *algo);
uint32_t page_load(lower_info *li, blockmanager *bm, algorithm *algo, FILE *fp){
	page_create_body(li, bm, algo);
	printf("now position :%lu\n", ftell(fp));

	/*read data in align_buffer*/
	fread(&a_buffer.idx, sizeof(uint8_t), 1, fp);
	char temp_data[PAGESIZE];
	for(uint32_t i=0; i<a_buffer.idx; i++){
		fread(temp_data, sizeof(char), PAGESIZE, fp);
		a_buffer.value[i]=inf_get_valueset(temp_data, PAGESIZE, FS_MALLOC_W);
	}
	fread(a_buffer.key, sizeof(uint32_t), a_buffer.idx, fp);


	/*read active and reserve segment*/
	pm_body *p=(pm_body*)calloc(sizeof(pm_body), 1);
	uint32_t active, reserve;
	fread(&active, sizeof(uint32_t), 1, fp);
	if(active!=UINT32_MAX){
		p->active=page_ftl.bm->get_segment_target(page_ftl.bm, active, BLOCK_LOAD);
	}
	else{
		p->active=NULL;
	}

	fread(&reserve, sizeof(uint32_t), 1, fp);
	if(reserve!=UINT32_MAX){
		p->reserve=page_ftl.bm->get_segment_target(page_ftl.bm, reserve, BLOCK_RESERVE);
	}
	else{
		p->reserve=NULL;
	}

	p->mapping=(uint32_t*)malloc(sizeof(uint32_t)*_NOP*L2PGAP);
	fread(p->mapping, sizeof(uint32_t), _NOP * L2PGAP, fp);
	page_ftl.algo_body=(void*)p;
	return 1;
}
