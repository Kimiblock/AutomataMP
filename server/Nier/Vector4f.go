// Code generated by the FlatBuffers compiler. DO NOT EDIT.

package Nier

import (
	flatbuffers "github.com/google/flatbuffers/go"
)

type Vector4f struct {
	_tab flatbuffers.Struct
}

func (rcv *Vector4f) Init(buf []byte, i flatbuffers.UOffsetT) {
	rcv._tab.Bytes = buf
	rcv._tab.Pos = i
}

func (rcv *Vector4f) Table() flatbuffers.Table {
	return rcv._tab.Table
}

func (rcv *Vector4f) X() float32 {
	return rcv._tab.GetFloat32(rcv._tab.Pos + flatbuffers.UOffsetT(0))
}
func (rcv *Vector4f) MutateX(n float32) bool {
	return rcv._tab.MutateFloat32(rcv._tab.Pos+flatbuffers.UOffsetT(0), n)
}

func (rcv *Vector4f) Y() float32 {
	return rcv._tab.GetFloat32(rcv._tab.Pos + flatbuffers.UOffsetT(4))
}
func (rcv *Vector4f) MutateY(n float32) bool {
	return rcv._tab.MutateFloat32(rcv._tab.Pos+flatbuffers.UOffsetT(4), n)
}

func (rcv *Vector4f) Z() float32 {
	return rcv._tab.GetFloat32(rcv._tab.Pos + flatbuffers.UOffsetT(8))
}
func (rcv *Vector4f) MutateZ(n float32) bool {
	return rcv._tab.MutateFloat32(rcv._tab.Pos+flatbuffers.UOffsetT(8), n)
}

func (rcv *Vector4f) W() float32 {
	return rcv._tab.GetFloat32(rcv._tab.Pos + flatbuffers.UOffsetT(12))
}
func (rcv *Vector4f) MutateW(n float32) bool {
	return rcv._tab.MutateFloat32(rcv._tab.Pos+flatbuffers.UOffsetT(12), n)
}

func CreateVector4f(builder *flatbuffers.Builder, x float32, y float32, z float32, w float32) flatbuffers.UOffsetT {
	builder.Prep(4, 16)
	builder.PrependFloat32(w)
	builder.PrependFloat32(z)
	builder.PrependFloat32(y)
	builder.PrependFloat32(x)
	return builder.Offset()
}
