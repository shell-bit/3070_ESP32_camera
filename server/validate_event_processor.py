"""Validate five-image preprocessing on CPU using local model assets; no model weights loaded."""
import json
import os
import tempfile
from pathlib import Path
from PIL import Image
from transformers import AutoProcessor
from qwen_vl_utils import process_vision_info

def main():
    with tempfile.TemporaryDirectory(prefix='event-preprocessor-') as folder:
        os.environ['CAMERA_DATA_DIR']=str(Path(folder)/'data')
        import worker
        model_path=os.environ.get('QWEN_MODEL_PATH','/root/autodl-tmp/models/Qwen2.5-VL-7B-Instruct')
        processor=AutoProcessor.from_pretrained(model_path,min_pixels=256*28*28,
            max_pixels=1024*28*28,local_files_only=True)
        paths=[];frames=[]
        for index in range(1,6):
            path=Path(folder)/(str(index)+'.jpg')
            Image.new('RGB',(320,240),(index*30,60,90)).save(path)
            paths.append(path)
            frames.append({'photo_index':index,'captured_at':None})
        model=worker.Models.__new__(worker.Models)
        evidence={}
        def prepare(messages):
            text=processor.apply_chat_template(messages,tokenize=False,add_generation_prompt=True)
            images,videos=process_vision_info(messages)
            inputs=processor(text=[text],images=images,videos=videos,padding=True,return_tensors='pt')
            assert len(images)==5 and inputs['image_grid_thw'].shape[0]==5
            assert inputs['input_ids'].device.type=='cpu' and inputs['pixel_values'].device.type=='cpu'
            evidence.update({'images':len(images),'input_tokens':inputs['input_ids'].shape[-1],
                             'image_grid_thw':inputs['image_grid_thw'].tolist(),'device':'cpu',
                             'model_weights_loaded':False})
            return '{"verdict":"uncertain","description":"Synthetic preprocessing check.","evidence_frame_indices":[]}'
        model._generate=prepare
        result=model.assess_event(paths,frames)
        assert result==('uncertain','Synthetic preprocessing check.',None,[])
        print(json.dumps(evidence))
if __name__=='__main__':main()
