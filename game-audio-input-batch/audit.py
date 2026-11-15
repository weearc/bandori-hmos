import json,re,pathlib
root=pathlib.Path(__file__).resolve().parents[1]
native=json.loads((root/'native-requirements.json').read_text(encoding='utf-8'))
lib=native['libraries'].get('libcri_ware_unity.so',{})
aa=lib.get('aaudio_symbol_strings',[])
report={'build':'GAME_AUDIO_INPUT_BATCH_V1','evidence':'native-requirements.json static strings; unity-report platform dlsym observations; probe touch source','audio':{'library':'libcri_ware_unity.so','cri_versions':lib.get('cri_version_strings',[]),'aaudio_symbols':aa,'groups':{'builder':[x for x in aa if 'Builder' in x],'stream':[x for x in aa if 'Stream' in x],'global':[x for x in aa if x.startswith('AAudio_')]},'opensles_candidates':['slCreateEngine','SL_IID_ENGINE','SL_IID_AUDIOIODEVICECAPABILITIES','SL_IID_ANDROIDCONFIGURATION','SL_IID_ANDROIDSIMPLEBUFFERQUEUE','SL_IID_ANDROIDSIMPLEBUFFERQUEUELOCATESOUND','SL_IID_ANDROIDACOUSTICECHOCANCELLATION','SL_IID_ANDROIDNOISESUPPRESSION','SL_IID_ANDROIDREVERB','SL_IID_ANDROIDFD']);

