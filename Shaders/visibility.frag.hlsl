struct PSOut { uint id : SV_Target0; };
PSOut main(uint visibility : TEXCOORD0) { PSOut o; o.id = visibility; return o; }
