"""Append the highest-scoring streaming window from every training negative."""
import argparse
from pathlib import Path
import numpy as np
import tensorflow as tf

def main():
    p=argparse.ArgumentParser();p.add_argument("--work",type=Path,required=True);a=p.parse_args()
    z=np.load(a.work/"training.npz");x,y=z["x"],z["y"]
    it=tf.lite.Interpreter(model_path=str(a.work/"hey_pog.tflite"));it.allocate_tensors()
    ii=it.get_input_details()[0];oo=it.get_output_details()[0];scale,zero=ii["quantization"]
    def quant(v): return np.clip(np.rint(v/scale+zero),-128,127).astype(np.int8)[None]
    mined=[]
    for number,s in enumerate(x[y==0]):
        it.reset_all_variables();ambient=quant(s[:3])
        for _ in range(100):it.set_tensor(ii["index"],ambient);it.invoke()
        scores=[]
        for i in range(0,198,3):
            it.set_tensor(ii["index"],quant(s[i:i+3]));it.invoke()
            scores.append(int(it.get_tensor(oo["index"]).reshape(-1)[0]))
        end=(int(np.argmax(scores))+1)*3
        history=np.concatenate((np.repeat(s[:1],200,axis=0),s[:end]))
        mined.append(history[-200:])
        if number%500==499:print("mined",number+1,flush=True)
    mined=np.asarray(mined,np.float32)
    np.savez(a.work/"training.npz",x=np.concatenate((x,mined)),
             y=np.concatenate((y,np.zeros(len(mined),np.float32))))
    print("appended",len(mined),"streaming hard negatives",flush=True)
if __name__=="__main__":main()
