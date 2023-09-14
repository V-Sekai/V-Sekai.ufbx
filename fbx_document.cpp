/**************************************************************************/
/*  gltf_document.cpp                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "fbx_document.h"

#include "core/config/project_settings.h"
#include "core/crypto/crypto_core.h"
#include "core/error/error_list.h"
#include "core/error/error_macros.h"
#include "core/io/config_file.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/file_access_memory.h"
#include "core/io/json.h"
#include "core/io/stream_peer.h"
#include "core/math/disjoint_set.h"
#include "core/string/print_string.h"
#include "core/version.h"
#include "drivers/png/png_driver_common.h"
#include "scene/3d/bone_attachment_3d.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/importer_mesh_instance_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/portable_compressed_texture.h"
#include "scene/resources/skin.h"
#include "scene/resources/surface_tool.h"

#include "modules/modules_enabled.gen.h" // For csg, gridmap.

#ifdef TOOLS_ENABLED
#include "editor/editor_file_system.h"
#endif
#ifdef MODULE_CSG_ENABLED
#include "modules/csg/csg_shape.h"
#endif // MODULE_CSG_ENABLED
#ifdef MODULE_GRIDMAP_ENABLED
#include "modules/gridmap/grid_map.h"
#endif // MODULE_GRIDMAP_ENABLED

// FIXME: Hardcoded to avoid editor dependency.
#define FBX_IMPORT_USE_NAMED_SKIN_BINDS 16
#define FBX_IMPORT_DISCARD_MESHES_AND_MATERIALS 32

#include "thirdparty/ufbx/ufbx.h"

#include <stdio.h>
#include <stdlib.h>
#include <cstdint>
#include <limits>

static Ref<ImporterMesh> _mesh_to_importer_mesh(Ref<Mesh> p_mesh) {
	Ref<ImporterMesh> importer_mesh;
	importer_mesh.instantiate();
	if (p_mesh.is_null()) {
		return importer_mesh;
	}

	Ref<ArrayMesh> array_mesh = p_mesh;
	if (p_mesh->get_blend_shape_count()) {
		ArrayMesh::BlendShapeMode shape_mode = ArrayMesh::BLEND_SHAPE_MODE_NORMALIZED;
		if (array_mesh.is_valid()) {
			shape_mode = array_mesh->get_blend_shape_mode();
		}
		importer_mesh->set_blend_shape_mode(shape_mode);
		for (int morph_i = 0; morph_i < p_mesh->get_blend_shape_count(); morph_i++) {
			importer_mesh->add_blend_shape(p_mesh->get_blend_shape_name(morph_i));
		}
	}
	for (int32_t surface_i = 0; surface_i < p_mesh->get_surface_count(); surface_i++) {
		Array array = p_mesh->surface_get_arrays(surface_i);
		Ref<Material> mat = p_mesh->surface_get_material(surface_i);
		String mat_name;
		if (mat.is_valid()) {
			mat_name = mat->get_name();
		} else {
			// Assign default material when no material is assigned.
			mat = Ref<StandardMaterial3D>(memnew(StandardMaterial3D));
		}
		importer_mesh->add_surface(p_mesh->surface_get_primitive_type(surface_i),
				array, p_mesh->surface_get_blend_shape_arrays(surface_i), p_mesh->surface_get_lods(surface_i), mat,
				mat_name, p_mesh->surface_get_format(surface_i));
	}
	return importer_mesh;
}

Error FBXDocument::_parse_json(const String &p_path, Ref<FBXState> p_state) {
	Error err;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ, &err);
	if (file.is_null()) {
		return err;
	}

	Vector<uint8_t> array;
	array.resize(file->get_length());
	file->get_buffer(array.ptrw(), array.size());
	String text;
	text.parse_utf8((const char *)array.ptr(), array.size());

	JSON json;
	err = json.parse(text);
	if (err != OK) {
		_err_print_error("", p_path.utf8().get_data(), json.get_error_line(), json.get_error_message().utf8().get_data(), false, ERR_HANDLER_SCRIPT);
		return err;
	}
	p_state->json = json.get_data();

	return OK;
}

Error FBXDocument::_parse_glb(Ref<FileAccess> p_file, Ref<FBXState> p_state) {
	ERR_FAIL_NULL_V(p_file, ERR_INVALID_PARAMETER);
	ERR_FAIL_NULL_V(p_state, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_file->get_position() != 0, ERR_FILE_CANT_READ);
	uint32_t magic = p_file->get_32();
	ERR_FAIL_COND_V(magic != 0x46546C67, ERR_FILE_UNRECOGNIZED); //glTF
	p_file->get_32(); // version
	p_file->get_32(); // length
	uint32_t chunk_length = p_file->get_32();
	uint32_t chunk_type = p_file->get_32();

	ERR_FAIL_COND_V(chunk_type != 0x4E4F534A, ERR_PARSE_ERROR); //JSON
	Vector<uint8_t> json_data;
	json_data.resize(chunk_length);
	uint32_t len = p_file->get_buffer(json_data.ptrw(), chunk_length);
	ERR_FAIL_COND_V(len != chunk_length, ERR_FILE_CORRUPT);

	String text;
	text.parse_utf8((const char *)json_data.ptr(), json_data.size());

	JSON json;
	Error err = json.parse(text);
	if (err != OK) {
		_err_print_error("", "", json.get_error_line(), json.get_error_message().utf8().get_data(), false, ERR_HANDLER_SCRIPT);
		return err;
	}

	p_state->json = json.get_data();

	//data?

	chunk_length = p_file->get_32();
	chunk_type = p_file->get_32();

	if (p_file->eof_reached()) {
		return OK; //all good
	}

	ERR_FAIL_COND_V(chunk_type != 0x004E4942, ERR_PARSE_ERROR); //BIN

	p_state->glb_data.resize(chunk_length);
	len = p_file->get_buffer(p_state->glb_data.ptrw(), chunk_length);
	ERR_FAIL_COND_V(len != chunk_length, ERR_FILE_CORRUPT);

	return OK;
}

static Array _vec3_to_arr(const Vector3 &p_vec3) {
	Array array;
	array.resize(3);
	array[0] = p_vec3.x;
	array[1] = p_vec3.y;
	array[2] = p_vec3.z;
	return array;
}

static Vector3 _arr_to_vec3(const Array &p_array) {
	ERR_FAIL_COND_V(p_array.size() != 3, Vector3());
	return Vector3(p_array[0], p_array[1], p_array[2]);
}

static Array _quaternion_to_array(const Quaternion &p_quaternion) {
	Array array;
	array.resize(4);
	array[0] = p_quaternion.x;
	array[1] = p_quaternion.y;
	array[2] = p_quaternion.z;
	array[3] = p_quaternion.w;
	return array;
}

static Quaternion _arr_to_quaternion(const Array &p_array) {
	ERR_FAIL_COND_V(p_array.size() != 4, Quaternion());
	return Quaternion(p_array[0], p_array[1], p_array[2], p_array[3]);
}

static Transform3D _arr_to_xform(const Array &p_array) {
	ERR_FAIL_COND_V(p_array.size() != 16, Transform3D());

	Transform3D xform;
	xform.basis.set_column(Vector3::AXIS_X, Vector3(p_array[0], p_array[1], p_array[2]));
	xform.basis.set_column(Vector3::AXIS_Y, Vector3(p_array[4], p_array[5], p_array[6]));
	xform.basis.set_column(Vector3::AXIS_Z, Vector3(p_array[8], p_array[9], p_array[10]));
	xform.set_origin(Vector3(p_array[12], p_array[13], p_array[14]));

	return xform;
}

static Vector<real_t> _xform_to_array(const Transform3D p_transform) {
	Vector<real_t> array;
	array.resize(16);
	Vector3 axis_x = p_transform.get_basis().get_column(Vector3::AXIS_X);
	array.write[0] = axis_x.x;
	array.write[1] = axis_x.y;
	array.write[2] = axis_x.z;
	array.write[3] = 0.0f;
	Vector3 axis_y = p_transform.get_basis().get_column(Vector3::AXIS_Y);
	array.write[4] = axis_y.x;
	array.write[5] = axis_y.y;
	array.write[6] = axis_y.z;
	array.write[7] = 0.0f;
	Vector3 axis_z = p_transform.get_basis().get_column(Vector3::AXIS_Z);
	array.write[8] = axis_z.x;
	array.write[9] = axis_z.y;
	array.write[10] = axis_z.z;
	array.write[11] = 0.0f;
	Vector3 origin = p_transform.get_origin();
	array.write[12] = origin.x;
	array.write[13] = origin.y;
	array.write[14] = origin.z;
	array.write[15] = 1.0f;
	return array;
}

String FBXDocument::_gen_unique_name(Ref<FBXState> p_state, const String &p_name) {
	const String s_name = p_name.validate_node_name();

	String u_name;
	int index = 1;
	while (true) {
		u_name = s_name;

		if (index > 1) {
			u_name += itos(index);
		}
		if (!p_state->unique_names.has(u_name)) {
			break;
		}
		index++;
	}

	p_state->unique_names.insert(u_name);

	return u_name;
}

String FBXDocument::_sanitize_animation_name(const String &p_name) {
	// Animations disallow the normal node invalid characters as well as  "," and "["
	// (See animation/animation_player.cpp::add_animation)

	// TODO: Consider adding invalid_characters or a validate_animation_name to animation_player to mirror Node.
	String anim_name = p_name.validate_node_name();
	anim_name = anim_name.replace(",", "");
	anim_name = anim_name.replace("[", "");
	return anim_name;
}

String FBXDocument::_gen_unique_animation_name(Ref<FBXState> p_state, const String &p_name) {
	const String s_name = _sanitize_animation_name(p_name);

	String u_name;
	int index = 1;
	while (true) {
		u_name = s_name;

		if (index > 1) {
			u_name += itos(index);
		}
		if (!p_state->unique_animation_names.has(u_name)) {
			break;
		}
		index++;
	}

	p_state->unique_animation_names.insert(u_name);

	return u_name;
}

String FBXDocument::_sanitize_bone_name(const String &p_name) {
	String bone_name = p_name;
	bone_name = bone_name.replace(":", "_");
	bone_name = bone_name.replace("/", "_");
	return bone_name;
}

String FBXDocument::_gen_unique_bone_name(Ref<FBXState> p_state, const FBXSkeletonIndex p_skel_i, const String &p_name) {
	String s_name = _sanitize_bone_name(p_name);
	if (s_name.is_empty()) {
		s_name = "bone";
	}
	String u_name;
	int index = 1;
	while (true) {
		u_name = s_name;

		if (index > 1) {
			u_name += "_" + itos(index);
		}
		if (!p_state->skeletons[p_skel_i]->unique_names.has(u_name)) {
			break;
		}
		index++;
	}

	p_state->skeletons.write[p_skel_i]->unique_names.insert(u_name);

	return u_name;
}

Error FBXDocument::_parse_scenes(Ref<FBXState> p_state) {
	p_state->unique_names.insert("Skeleton3D"); // Reserve skeleton name.
	ERR_FAIL_COND_V(!p_state->json.has("scenes"), ERR_FILE_CORRUPT);
	const Array &scenes = p_state->json["scenes"];
	int loaded_scene = 0;
	if (p_state->json.has("scene")) {
		loaded_scene = p_state->json["scene"];
	} else {
		WARN_PRINT("The load-time scene is not defined in the glTF2 file. Picking the first scene.");
	}

	if (scenes.size()) {
		ERR_FAIL_COND_V(loaded_scene >= scenes.size(), ERR_FILE_CORRUPT);
		const Dictionary &s = scenes[loaded_scene];
		ERR_FAIL_COND_V(!s.has("nodes"), ERR_UNAVAILABLE);
		const Array &nodes = s["nodes"];
		for (int j = 0; j < nodes.size(); j++) {
			p_state->root_nodes.push_back(nodes[j]);
		}

		if (s.has("name") && !String(s["name"]).is_empty() && !((String)s["name"]).begins_with("Scene")) {
			p_state->scene_name = _gen_unique_name(p_state, s["name"]);
		} else {
			p_state->scene_name = _gen_unique_name(p_state, p_state->filename);
		}
	}

	return OK;
}

Error FBXDocument::_parse_nodes(Ref<FBXState> p_state) {
	ERR_FAIL_COND_V(!p_state->json.has("nodes"), ERR_FILE_CORRUPT);
	const Array &nodes = p_state->json["nodes"];
	for (int i = 0; i < nodes.size(); i++) {
		Ref<FBXNode> node;
		node.instantiate();
		const Dictionary &n = nodes[i];

		if (n.has("name")) {
			node->set_name(n["name"]);
		}
		if (n.has("camera")) {
			node->camera = n["camera"];
		}
		if (n.has("mesh")) {
			node->mesh = n["mesh"];
		}
		if (n.has("skin")) {
			node->skin = n["skin"];
		}
		if (n.has("matrix")) {
			node->xform = _arr_to_xform(n["matrix"]);
		} else {
			if (n.has("translation")) {
				node->position = _arr_to_vec3(n["translation"]);
			}
			if (n.has("rotation")) {
				node->rotation = _arr_to_quaternion(n["rotation"]);
			}
			if (n.has("scale")) {
				node->scale = _arr_to_vec3(n["scale"]);
			}

			node->xform.basis.set_quaternion_scale(node->rotation, node->scale);
			node->xform.origin = node->position;
		}

		if (n.has("children")) {
			const Array &children = n["children"];
			for (int j = 0; j < children.size(); j++) {
				node->children.push_back(children[j]);
			}
		}

		p_state->nodes.push_back(node);
	}

	// build the hierarchy
	for (FBXNodeIndex node_i = 0; node_i < p_state->nodes.size(); node_i++) {
		for (int j = 0; j < p_state->nodes[node_i]->children.size(); j++) {
			FBXNodeIndex child_i = p_state->nodes[node_i]->children[j];

			ERR_FAIL_INDEX_V(child_i, p_state->nodes.size(), ERR_FILE_CORRUPT);
			ERR_CONTINUE(p_state->nodes[child_i]->parent != -1); //node already has a parent, wtf.

			p_state->nodes.write[child_i]->parent = node_i;
		}
	}

	_compute_node_heights(p_state);

	return OK;
}

void FBXDocument::_compute_node_heights(Ref<FBXState> p_state) {
	p_state->root_nodes.clear();
	for (FBXNodeIndex node_i = 0; node_i < p_state->nodes.size(); ++node_i) {
		Ref<FBXNode> node = p_state->nodes[node_i];
		node->height = 0;

		FBXNodeIndex current_i = node_i;
		while (current_i >= 0) {
			const FBXNodeIndex parent_i = p_state->nodes[current_i]->parent;
			if (parent_i >= 0) {
				++node->height;
			}
			current_i = parent_i;
		}

		if (node->height == 0) {
			p_state->root_nodes.push_back(node_i);
		}
	}
}

static Vector<uint8_t> _parse_base64_uri(const String &p_uri) {
	int start = p_uri.find(",");
	ERR_FAIL_COND_V(start == -1, Vector<uint8_t>());

	CharString substr = p_uri.substr(start + 1).ascii();

	int strlen = substr.length();

	Vector<uint8_t> buf;
	buf.resize(strlen / 4 * 3 + 1 + 1);

	size_t len = 0;
	ERR_FAIL_COND_V(CryptoCore::b64_decode(buf.ptrw(), buf.size(), &len, (unsigned char *)substr.get_data(), strlen) != OK, Vector<uint8_t>());

	buf.resize(len);

	return buf;
}

Error FBXDocument::_encode_buffer_glb(Ref<FBXState> p_state, const String &p_path) {
	print_verbose("glTF: Total buffers: " + itos(p_state->buffers.size()));

	if (!p_state->buffers.size()) {
		return OK;
	}
	Array buffers;
	if (p_state->buffers.size()) {
		Vector<uint8_t> buffer_data = p_state->buffers[0];
		Dictionary gltf_buffer;

		gltf_buffer["byteLength"] = buffer_data.size();
		buffers.push_back(gltf_buffer);
	}

	for (FBXBufferIndex i = 1; i < p_state->buffers.size() - 1; i++) {
		Vector<uint8_t> buffer_data = p_state->buffers[i];
		Dictionary gltf_buffer;
		String filename = p_path.get_basename().get_file() + itos(i) + ".bin";
		String path = p_path.get_base_dir() + "/" + filename;
		Error err;
		Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE, &err);
		if (file.is_null()) {
			return err;
		}
		if (buffer_data.size() == 0) {
			return OK;
		}
		file->create(FileAccess::ACCESS_RESOURCES);
		file->store_buffer(buffer_data.ptr(), buffer_data.size());
		gltf_buffer["uri"] = filename;
		gltf_buffer["byteLength"] = buffer_data.size();
		buffers.push_back(gltf_buffer);
	}
	p_state->json["buffers"] = buffers;

	return OK;
}

Error FBXDocument::_encode_buffer_bins(Ref<FBXState> p_state, const String &p_path) {
	print_verbose("glTF: Total buffers: " + itos(p_state->buffers.size()));

	if (!p_state->buffers.size()) {
		return OK;
	}
	Array buffers;

	for (FBXBufferIndex i = 0; i < p_state->buffers.size(); i++) {
		Vector<uint8_t> buffer_data = p_state->buffers[i];
		Dictionary gltf_buffer;
		String filename = p_path.get_basename().get_file() + itos(i) + ".bin";
		String path = p_path.get_base_dir() + "/" + filename;
		Error err;
		Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE, &err);
		if (file.is_null()) {
			return err;
		}
		if (buffer_data.size() == 0) {
			return OK;
		}
		file->create(FileAccess::ACCESS_RESOURCES);
		file->store_buffer(buffer_data.ptr(), buffer_data.size());
		gltf_buffer["uri"] = filename;
		gltf_buffer["byteLength"] = buffer_data.size();
		buffers.push_back(gltf_buffer);
	}
	p_state->json["buffers"] = buffers;

	return OK;
}

Error FBXDocument::_parse_buffers(Ref<FBXState> p_state, const String &p_base_path) {
	if (!p_state->json.has("buffers")) {
		return OK;
	}

	const Array &buffers = p_state->json["buffers"];
	for (FBXBufferIndex i = 0; i < buffers.size(); i++) {
		if (i == 0 && p_state->glb_data.size()) {
			p_state->buffers.push_back(p_state->glb_data);

		} else {
			const Dictionary &buffer = buffers[i];
			if (buffer.has("uri")) {
				Vector<uint8_t> buffer_data;
				String uri = buffer["uri"];

				if (uri.begins_with("data:")) { // Embedded data using base64.
					// Validate data MIME types and throw an error if it's one we don't know/support.
					if (!uri.begins_with("data:application/octet-stream;base64") &&
							!uri.begins_with("data:application/gltf-buffer;base64")) {
						ERR_PRINT("glTF: Got buffer with an unknown URI data type: " + uri);
					}
					buffer_data = _parse_base64_uri(uri);
				} else { // Relative path to an external image file.
					ERR_FAIL_COND_V(p_base_path.is_empty(), ERR_INVALID_PARAMETER);
					uri = uri.uri_decode();
					uri = p_base_path.path_join(uri).replace("\\", "/"); // Fix for Windows.
					buffer_data = FileAccess::get_file_as_bytes(uri);
					ERR_FAIL_COND_V_MSG(buffer.size() == 0, ERR_PARSE_ERROR, "glTF: Couldn't load binary file as an array: " + uri);
				}

				ERR_FAIL_COND_V(!buffer.has("byteLength"), ERR_PARSE_ERROR);
				int byteLength = buffer["byteLength"];
				ERR_FAIL_COND_V(byteLength < buffer_data.size(), ERR_PARSE_ERROR);
				p_state->buffers.push_back(buffer_data);
			}
		}
	}

	print_verbose("glTF: Total buffers: " + itos(p_state->buffers.size()));

	return OK;
}

Error FBXDocument::_encode_buffer_views(Ref<FBXState> p_state) {
	Array buffers;
	for (FBXBufferViewIndex i = 0; i < p_state->buffer_views.size(); i++) {
		Dictionary d;

		Ref<FBXBufferView> buffer_view = p_state->buffer_views[i];

		d["buffer"] = buffer_view->buffer;
		d["byteLength"] = buffer_view->byte_length;

		d["byteOffset"] = buffer_view->byte_offset;

		if (buffer_view->byte_stride != -1) {
			d["byteStride"] = buffer_view->byte_stride;
		}

		// TODO Sparse
		// d["target"] = buffer_view->indices;

		ERR_FAIL_COND_V(!d.has("buffer"), ERR_INVALID_DATA);
		ERR_FAIL_COND_V(!d.has("byteLength"), ERR_INVALID_DATA);
		buffers.push_back(d);
	}
	print_verbose("glTF: Total buffer views: " + itos(p_state->buffer_views.size()));
	if (!buffers.size()) {
		return OK;
	}
	p_state->json["bufferViews"] = buffers;
	return OK;
}

Error FBXDocument::_parse_buffer_views(Ref<FBXState> p_state) {
	if (!p_state->json.has("bufferViews")) {
		return OK;
	}
	const Array &buffers = p_state->json["bufferViews"];
	for (FBXBufferViewIndex i = 0; i < buffers.size(); i++) {
		const Dictionary &d = buffers[i];

		Ref<FBXBufferView> buffer_view;
		buffer_view.instantiate();

		ERR_FAIL_COND_V(!d.has("buffer"), ERR_PARSE_ERROR);
		buffer_view->buffer = d["buffer"];
		ERR_FAIL_COND_V(!d.has("byteLength"), ERR_PARSE_ERROR);
		buffer_view->byte_length = d["byteLength"];

		if (d.has("byteOffset")) {
			buffer_view->byte_offset = d["byteOffset"];
		}

		if (d.has("byteStride")) {
			buffer_view->byte_stride = d["byteStride"];
		}

		if (d.has("target")) {
			const int target = d["target"];
			buffer_view->indices = target == FBXDocument::ELEMENT_ARRAY_BUFFER;
		}

		p_state->buffer_views.push_back(buffer_view);
	}

	print_verbose("glTF: Total buffer views: " + itos(p_state->buffer_views.size()));

	return OK;
}

Error FBXDocument::_encode_accessors(Ref<FBXState> p_state) {
	Array accessors;
	for (FBXAccessorIndex i = 0; i < p_state->accessors.size(); i++) {
		Dictionary d;

		Ref<FBXAccessor> accessor = p_state->accessors[i];
		d["componentType"] = accessor->component_type;
		d["count"] = accessor->count;
		d["type"] = _get_accessor_type_name(accessor->type);
		d["byteOffset"] = accessor->byte_offset;
		d["normalized"] = accessor->normalized;
		d["max"] = accessor->max;
		d["min"] = accessor->min;
		d["bufferView"] = accessor->buffer_view; //optional because it may be sparse...

		// Dictionary s;
		// s["count"] = accessor->sparse_count;
		// ERR_FAIL_COND_V(!s.has("count"), ERR_PARSE_ERROR);

		// s["indices"] = accessor->sparse_accessors;
		// ERR_FAIL_COND_V(!s.has("indices"), ERR_PARSE_ERROR);

		// Dictionary si;

		// si["bufferView"] = accessor->sparse_indices_buffer_view;

		// ERR_FAIL_COND_V(!si.has("bufferView"), ERR_PARSE_ERROR);
		// si["componentType"] = accessor->sparse_indices_component_type;

		// if (si.has("byteOffset")) {
		// 	si["byteOffset"] = accessor->sparse_indices_byte_offset;
		// }

		// ERR_FAIL_COND_V(!si.has("componentType"), ERR_PARSE_ERROR);
		// s["indices"] = si;
		// Dictionary sv;

		// sv["bufferView"] = accessor->sparse_values_buffer_view;
		// if (sv.has("byteOffset")) {
		// 	sv["byteOffset"] = accessor->sparse_values_byte_offset;
		// }
		// ERR_FAIL_COND_V(!sv.has("bufferView"), ERR_PARSE_ERROR);
		// s["values"] = sv;
		// ERR_FAIL_COND_V(!s.has("values"), ERR_PARSE_ERROR);
		// d["sparse"] = s;
		accessors.push_back(d);
	}

	if (!accessors.size()) {
		return OK;
	}
	p_state->json["accessors"] = accessors;
	ERR_FAIL_COND_V(!p_state->json.has("accessors"), ERR_FILE_CORRUPT);
	print_verbose("glTF: Total accessors: " + itos(p_state->accessors.size()));

	return OK;
}

String FBXDocument::_get_accessor_type_name(const FBXType p_type) {
	if (p_type == FBXType::TYPE_SCALAR) {
		return "SCALAR";
	}
	if (p_type == FBXType::TYPE_VEC2) {
		return "VEC2";
	}
	if (p_type == FBXType::TYPE_VEC3) {
		return "VEC3";
	}
	if (p_type == FBXType::TYPE_VEC4) {
		return "VEC4";
	}

	if (p_type == FBXType::TYPE_MAT2) {
		return "MAT2";
	}
	if (p_type == FBXType::TYPE_MAT3) {
		return "MAT3";
	}
	if (p_type == FBXType::TYPE_MAT4) {
		return "MAT4";
	}
	ERR_FAIL_V("SCALAR");
}

FBXType FBXDocument::_get_type_from_str(const String &p_string) {
	if (p_string == "SCALAR") {
		return FBXType::TYPE_SCALAR;
	}

	if (p_string == "VEC2") {
		return FBXType::TYPE_VEC2;
	}
	if (p_string == "VEC3") {
		return FBXType::TYPE_VEC3;
	}
	if (p_string == "VEC4") {
		return FBXType::TYPE_VEC4;
	}

	if (p_string == "MAT2") {
		return FBXType::TYPE_MAT2;
	}
	if (p_string == "MAT3") {
		return FBXType::TYPE_MAT3;
	}
	if (p_string == "MAT4") {
		return FBXType::TYPE_MAT4;
	}

	ERR_FAIL_V(FBXType::TYPE_SCALAR);
}

Error FBXDocument::_parse_accessors(Ref<FBXState> p_state) {
	if (!p_state->json.has("accessors")) {
		return OK;
	}
	const Array &accessors = p_state->json["accessors"];
	for (FBXAccessorIndex i = 0; i < accessors.size(); i++) {
		const Dictionary &d = accessors[i];

		Ref<FBXAccessor> accessor;
		accessor.instantiate();

		ERR_FAIL_COND_V(!d.has("componentType"), ERR_PARSE_ERROR);
		accessor->component_type = d["componentType"];
		ERR_FAIL_COND_V(!d.has("count"), ERR_PARSE_ERROR);
		accessor->count = d["count"];
		ERR_FAIL_COND_V(!d.has("type"), ERR_PARSE_ERROR);
		accessor->type = _get_type_from_str(d["type"]);

		if (d.has("bufferView")) {
			accessor->buffer_view = d["bufferView"]; //optional because it may be sparse...
		}

		if (d.has("byteOffset")) {
			accessor->byte_offset = d["byteOffset"];
		}

		if (d.has("normalized")) {
			accessor->normalized = d["normalized"];
		}

		if (d.has("max")) {
			accessor->max = d["max"];
		}

		if (d.has("min")) {
			accessor->min = d["min"];
		}

		if (d.has("sparse")) {
			//eeh..

			const Dictionary &s = d["sparse"];

			ERR_FAIL_COND_V(!s.has("count"), ERR_PARSE_ERROR);
			accessor->sparse_count = s["count"];
			ERR_FAIL_COND_V(!s.has("indices"), ERR_PARSE_ERROR);
			const Dictionary &si = s["indices"];

			ERR_FAIL_COND_V(!si.has("bufferView"), ERR_PARSE_ERROR);
			accessor->sparse_indices_buffer_view = si["bufferView"];
			ERR_FAIL_COND_V(!si.has("componentType"), ERR_PARSE_ERROR);
			accessor->sparse_indices_component_type = si["componentType"];

			if (si.has("byteOffset")) {
				accessor->sparse_indices_byte_offset = si["byteOffset"];
			}

			ERR_FAIL_COND_V(!s.has("values"), ERR_PARSE_ERROR);
			const Dictionary &sv = s["values"];

			ERR_FAIL_COND_V(!sv.has("bufferView"), ERR_PARSE_ERROR);
			accessor->sparse_values_buffer_view = sv["bufferView"];
			if (sv.has("byteOffset")) {
				accessor->sparse_values_byte_offset = sv["byteOffset"];
			}
		}

		p_state->accessors.push_back(accessor);
	}

	print_verbose("glTF: Total accessors: " + itos(p_state->accessors.size()));

	return OK;
}

double FBXDocument::_filter_number(double p_float) {
	if (Math::is_nan(p_float)) {
		return 0.0f;
	}
	return p_float;
}

String FBXDocument::_get_component_type_name(const uint32_t p_component) {
	switch (p_component) {
		case FBXDocument::COMPONENT_TYPE_BYTE:
			return "Byte";
		case FBXDocument::COMPONENT_TYPE_UNSIGNED_BYTE:
			return "UByte";
		case FBXDocument::COMPONENT_TYPE_SHORT:
			return "Short";
		case FBXDocument::COMPONENT_TYPE_UNSIGNED_SHORT:
			return "UShort";
		case FBXDocument::COMPONENT_TYPE_INT:
			return "Int";
		case FBXDocument::COMPONENT_TYPE_FLOAT:
			return "Float";
	}

	return "<Error>";
}

String FBXDocument::_get_type_name(const FBXType p_component) {
	static const char *names[] = {
		"float",
		"vec2",
		"vec3",
		"vec4",
		"mat2",
		"mat3",
		"mat4"
	};

	return names[p_component];
}

Error FBXDocument::_encode_buffer_view(Ref<FBXState> p_state, const double *p_src, const int p_count, const FBXType p_type, const int p_component_type, const bool p_normalized, const int p_byte_offset, const bool p_for_vertex, FBXBufferViewIndex &r_accessor) {
	const int component_count_for_type[7] = {
		1, 2, 3, 4, 4, 9, 16
	};

	const int component_count = component_count_for_type[p_type];
	const int component_size = _get_component_type_size(p_component_type);
	ERR_FAIL_COND_V(component_size == 0, FAILED);

	int skip_every = 0;
	int skip_bytes = 0;
	//special case of alignments, as described in spec
	switch (p_component_type) {
		case COMPONENT_TYPE_BYTE:
		case COMPONENT_TYPE_UNSIGNED_BYTE: {
			if (p_type == TYPE_MAT2) {
				skip_every = 2;
				skip_bytes = 2;
			}
			if (p_type == TYPE_MAT3) {
				skip_every = 3;
				skip_bytes = 1;
			}
		} break;
		case COMPONENT_TYPE_SHORT:
		case COMPONENT_TYPE_UNSIGNED_SHORT: {
			if (p_type == TYPE_MAT3) {
				skip_every = 6;
				skip_bytes = 4;
			}
		} break;
		default: {
		}
	}

	Ref<FBXBufferView> bv;
	bv.instantiate();
	const uint32_t offset = bv->byte_offset = p_byte_offset;
	Vector<uint8_t> &gltf_buffer = p_state->buffers.write[0];

	int stride = _get_component_type_size(p_component_type);
	if (p_for_vertex && stride % 4) {
		stride += 4 - (stride % 4); //according to spec must be multiple of 4
	}
	//use to debug
	print_verbose("glTF: encoding type " + _get_type_name(p_type) + " component type: " + _get_component_type_name(p_component_type) + " stride: " + itos(stride) + " amount " + itos(p_count));

	print_verbose("glTF: encoding accessor offset " + itos(p_byte_offset) + " view offset: " + itos(bv->byte_offset) + " total buffer len: " + itos(gltf_buffer.size()) + " view len " + itos(bv->byte_length));

	const int buffer_end = (stride * (p_count - 1)) + _get_component_type_size(p_component_type);
	// TODO define bv->byte_stride
	bv->byte_offset = gltf_buffer.size();

	switch (p_component_type) {
		case COMPONENT_TYPE_BYTE: {
			Vector<int8_t> buffer;
			buffer.resize(p_count * component_count);
			int32_t dst_i = 0;
			for (int i = 0; i < p_count; i++) {
				for (int j = 0; j < component_count; j++) {
					if (skip_every && j > 0 && (j % skip_every) == 0) {
						dst_i += skip_bytes;
					}
					double d = *p_src;
					if (p_normalized) {
						buffer.write[dst_i] = d * 128.0;
					} else {
						buffer.write[dst_i] = d;
					}
					p_src++;
					dst_i++;
				}
			}
			int64_t old_size = gltf_buffer.size();
			gltf_buffer.resize(old_size + (buffer.size() * sizeof(int8_t)));
			memcpy(gltf_buffer.ptrw() + old_size, buffer.ptrw(), buffer.size() * sizeof(int8_t));
			bv->byte_length = buffer.size() * sizeof(int8_t);
		} break;
		case COMPONENT_TYPE_UNSIGNED_BYTE: {
			Vector<uint8_t> buffer;
			buffer.resize(p_count * component_count);
			int32_t dst_i = 0;
			for (int i = 0; i < p_count; i++) {
				for (int j = 0; j < component_count; j++) {
					if (skip_every && j > 0 && (j % skip_every) == 0) {
						dst_i += skip_bytes;
					}
					double d = *p_src;
					if (p_normalized) {
						buffer.write[dst_i] = d * 255.0;
					} else {
						buffer.write[dst_i] = d;
					}
					p_src++;
					dst_i++;
				}
			}
			gltf_buffer.append_array(buffer);
			bv->byte_length = buffer.size() * sizeof(uint8_t);
		} break;
		case COMPONENT_TYPE_SHORT: {
			Vector<int16_t> buffer;
			buffer.resize(p_count * component_count);
			int32_t dst_i = 0;
			for (int i = 0; i < p_count; i++) {
				for (int j = 0; j < component_count; j++) {
					if (skip_every && j > 0 && (j % skip_every) == 0) {
						dst_i += skip_bytes;
					}
					double d = *p_src;
					if (p_normalized) {
						buffer.write[dst_i] = d * 32768.0;
					} else {
						buffer.write[dst_i] = d;
					}
					p_src++;
					dst_i++;
				}
			}
			int64_t old_size = gltf_buffer.size();
			gltf_buffer.resize(old_size + (buffer.size() * sizeof(int16_t)));
			memcpy(gltf_buffer.ptrw() + old_size, buffer.ptrw(), buffer.size() * sizeof(int16_t));
			bv->byte_length = buffer.size() * sizeof(int16_t);
		} break;
		case COMPONENT_TYPE_UNSIGNED_SHORT: {
			Vector<uint16_t> buffer;
			buffer.resize(p_count * component_count);
			int32_t dst_i = 0;
			for (int i = 0; i < p_count; i++) {
				for (int j = 0; j < component_count; j++) {
					if (skip_every && j > 0 && (j % skip_every) == 0) {
						dst_i += skip_bytes;
					}
					double d = *p_src;
					if (p_normalized) {
						buffer.write[dst_i] = d * 65535.0;
					} else {
						buffer.write[dst_i] = d;
					}
					p_src++;
					dst_i++;
				}
			}
			int64_t old_size = gltf_buffer.size();
			gltf_buffer.resize(old_size + (buffer.size() * sizeof(uint16_t)));
			memcpy(gltf_buffer.ptrw() + old_size, buffer.ptrw(), buffer.size() * sizeof(uint16_t));
			bv->byte_length = buffer.size() * sizeof(uint16_t);
		} break;
		case COMPONENT_TYPE_INT: {
			Vector<int> buffer;
			buffer.resize(p_count * component_count);
			int32_t dst_i = 0;
			for (int i = 0; i < p_count; i++) {
				for (int j = 0; j < component_count; j++) {
					if (skip_every && j > 0 && (j % skip_every) == 0) {
						dst_i += skip_bytes;
					}
					double d = *p_src;
					buffer.write[dst_i] = d;
					p_src++;
					dst_i++;
				}
			}
			int64_t old_size = gltf_buffer.size();
			gltf_buffer.resize(old_size + (buffer.size() * sizeof(int32_t)));
			memcpy(gltf_buffer.ptrw() + old_size, buffer.ptrw(), buffer.size() * sizeof(int32_t));
			bv->byte_length = buffer.size() * sizeof(int32_t);
		} break;
		case COMPONENT_TYPE_FLOAT: {
			Vector<float> buffer;
			buffer.resize(p_count * component_count);
			int32_t dst_i = 0;
			for (int i = 0; i < p_count; i++) {
				for (int j = 0; j < component_count; j++) {
					if (skip_every && j > 0 && (j % skip_every) == 0) {
						dst_i += skip_bytes;
					}
					double d = *p_src;
					buffer.write[dst_i] = d;
					p_src++;
					dst_i++;
				}
			}
			int64_t old_size = gltf_buffer.size();
			gltf_buffer.resize(old_size + (buffer.size() * sizeof(float)));
			memcpy(gltf_buffer.ptrw() + old_size, buffer.ptrw(), buffer.size() * sizeof(float));
			bv->byte_length = buffer.size() * sizeof(float);
		} break;
	}
	ERR_FAIL_COND_V(buffer_end > bv->byte_length, ERR_INVALID_DATA);

	ERR_FAIL_COND_V((int)(offset + buffer_end) > gltf_buffer.size(), ERR_INVALID_DATA);
	r_accessor = bv->buffer = p_state->buffer_views.size();
	p_state->buffer_views.push_back(bv);
	return OK;
}

Error FBXDocument::_decode_buffer_view(Ref<FBXState> p_state, double *p_dst, const FBXBufferViewIndex p_buffer_view, const int p_skip_every, const int p_skip_bytes, const int p_element_size, const int p_count, const FBXType p_type, const int p_component_count, const int p_component_type, const int p_component_size, const bool p_normalized, const int p_byte_offset, const bool p_for_vertex) {
	const Ref<FBXBufferView> bv = p_state->buffer_views[p_buffer_view];

	int stride = p_element_size;
	if (bv->byte_stride != -1) {
		stride = bv->byte_stride;
	}
	if (p_for_vertex && stride % 4) {
		stride += 4 - (stride % 4); //according to spec must be multiple of 4
	}

	ERR_FAIL_INDEX_V(bv->buffer, p_state->buffers.size(), ERR_PARSE_ERROR);

	const uint32_t offset = bv->byte_offset + p_byte_offset;
	Vector<uint8_t> buffer = p_state->buffers[bv->buffer]; //copy on write, so no performance hit
	const uint8_t *bufptr = buffer.ptr();

	//use to debug
	print_verbose("glTF: type " + _get_type_name(p_type) + " component type: " + _get_component_type_name(p_component_type) + " stride: " + itos(stride) + " amount " + itos(p_count));
	print_verbose("glTF: accessor offset " + itos(p_byte_offset) + " view offset: " + itos(bv->byte_offset) + " total buffer len: " + itos(buffer.size()) + " view len " + itos(bv->byte_length));

	const int buffer_end = (stride * (p_count - 1)) + p_element_size;
	ERR_FAIL_COND_V(buffer_end > bv->byte_length, ERR_PARSE_ERROR);

	ERR_FAIL_COND_V((int)(offset + buffer_end) > buffer.size(), ERR_PARSE_ERROR);

	//fill everything as doubles

	for (int i = 0; i < p_count; i++) {
		const uint8_t *src = &bufptr[offset + i * stride];

		for (int j = 0; j < p_component_count; j++) {
			if (p_skip_every && j > 0 && (j % p_skip_every) == 0) {
				src += p_skip_bytes;
			}

			double d = 0;

			switch (p_component_type) {
				case COMPONENT_TYPE_BYTE: {
					int8_t b = int8_t(*src);
					if (p_normalized) {
						d = (double(b) / 128.0);
					} else {
						d = double(b);
					}
				} break;
				case COMPONENT_TYPE_UNSIGNED_BYTE: {
					uint8_t b = *src;
					if (p_normalized) {
						d = (double(b) / 255.0);
					} else {
						d = double(b);
					}
				} break;
				case COMPONENT_TYPE_SHORT: {
					int16_t s = *(int16_t *)src;
					if (p_normalized) {
						d = (double(s) / 32768.0);
					} else {
						d = double(s);
					}
				} break;
				case COMPONENT_TYPE_UNSIGNED_SHORT: {
					uint16_t s = *(uint16_t *)src;
					if (p_normalized) {
						d = (double(s) / 65535.0);
					} else {
						d = double(s);
					}
				} break;
				case COMPONENT_TYPE_INT: {
					d = *(int *)src;
				} break;
				case COMPONENT_TYPE_FLOAT: {
					d = *(float *)src;
				} break;
			}

			*p_dst++ = d;
			src += p_component_size;
		}
	}

	return OK;
}

int FBXDocument::_get_component_type_size(const int p_component_type) {
	switch (p_component_type) {
		case COMPONENT_TYPE_BYTE:
		case COMPONENT_TYPE_UNSIGNED_BYTE:
			return 1;
			break;
		case COMPONENT_TYPE_SHORT:
		case COMPONENT_TYPE_UNSIGNED_SHORT:
			return 2;
			break;
		case COMPONENT_TYPE_INT:
		case COMPONENT_TYPE_FLOAT:
			return 4;
			break;
		default: {
			ERR_FAIL_V(0);
		}
	}
	return 0;
}

Vector<double> FBXDocument::_decode_accessor(Ref<FBXState> p_state, const FBXAccessorIndex p_accessor, const bool p_for_vertex) {
	//spec, for reference:
	//https://github.com/KhronosGroup/glTF/tree/master/specification/2.0#data-alignment

	ERR_FAIL_INDEX_V(p_accessor, p_state->accessors.size(), Vector<double>());

	const Ref<FBXAccessor> a = p_state->accessors[p_accessor];

	const int component_count_for_type[7] = {
		1, 2, 3, 4, 4, 9, 16
	};

	const int component_count = component_count_for_type[a->type];
	const int component_size = _get_component_type_size(a->component_type);
	ERR_FAIL_COND_V(component_size == 0, Vector<double>());
	int element_size = component_count * component_size;

	int skip_every = 0;
	int skip_bytes = 0;
	//special case of alignments, as described in spec
	switch (a->component_type) {
		case COMPONENT_TYPE_BYTE:
		case COMPONENT_TYPE_UNSIGNED_BYTE: {
			if (a->type == TYPE_MAT2) {
				skip_every = 2;
				skip_bytes = 2;
				element_size = 8; //override for this case
			}
			if (a->type == TYPE_MAT3) {
				skip_every = 3;
				skip_bytes = 1;
				element_size = 12; //override for this case
			}
		} break;
		case COMPONENT_TYPE_SHORT:
		case COMPONENT_TYPE_UNSIGNED_SHORT: {
			if (a->type == TYPE_MAT3) {
				skip_every = 6;
				skip_bytes = 4;
				element_size = 16; //override for this case
			}
		} break;
		default: {
		}
	}

	Vector<double> dst_buffer;
	dst_buffer.resize(component_count * a->count);
	double *dst = dst_buffer.ptrw();

	if (a->buffer_view >= 0) {
		ERR_FAIL_INDEX_V(a->buffer_view, p_state->buffer_views.size(), Vector<double>());

		const Error err = _decode_buffer_view(p_state, dst, a->buffer_view, skip_every, skip_bytes, element_size, a->count, a->type, component_count, a->component_type, component_size, a->normalized, a->byte_offset, p_for_vertex);
		if (err != OK) {
			return Vector<double>();
		}
	} else {
		//fill with zeros, as bufferview is not defined.
		for (int i = 0; i < (a->count * component_count); i++) {
			dst_buffer.write[i] = 0;
		}
	}

	if (a->sparse_count > 0) {
		// I could not find any file using this, so this code is so far untested
		Vector<double> indices;
		indices.resize(a->sparse_count);
		const int indices_component_size = _get_component_type_size(a->sparse_indices_component_type);

		Error err = _decode_buffer_view(p_state, indices.ptrw(), a->sparse_indices_buffer_view, 0, 0, indices_component_size, a->sparse_count, TYPE_SCALAR, 1, a->sparse_indices_component_type, indices_component_size, false, a->sparse_indices_byte_offset, false);
		if (err != OK) {
			return Vector<double>();
		}

		Vector<double> data;
		data.resize(component_count * a->sparse_count);
		err = _decode_buffer_view(p_state, data.ptrw(), a->sparse_values_buffer_view, skip_every, skip_bytes, element_size, a->sparse_count, a->type, component_count, a->component_type, component_size, a->normalized, a->sparse_values_byte_offset, p_for_vertex);
		if (err != OK) {
			return Vector<double>();
		}

		for (int i = 0; i < indices.size(); i++) {
			const int write_offset = int(indices[i]) * component_count;

			for (int j = 0; j < component_count; j++) {
				dst[write_offset + j] = data[i * component_count + j];
			}
		}
	}

	return dst_buffer;
}

FBXAccessorIndex FBXDocument::_encode_accessor_as_ints(Ref<FBXState> p_state, const Vector<int32_t> p_attribs, const bool p_for_vertex) {
	if (p_attribs.size() == 0) {
		return -1;
	}
	const int element_count = 1;
	const int ret_size = p_attribs.size();
	Vector<double> attribs;
	attribs.resize(ret_size);
	Vector<double> type_max;
	type_max.resize(element_count);
	Vector<double> type_min;
	type_min.resize(element_count);
	for (int i = 0; i < p_attribs.size(); i++) {
		attribs.write[i] = Math::snapped(p_attribs[i], 1.0);
		if (i == 0) {
			for (int32_t type_i = 0; type_i < element_count; type_i++) {
				type_max.write[type_i] = attribs[(i * element_count) + type_i];
				type_min.write[type_i] = attribs[(i * element_count) + type_i];
			}
		}
		for (int32_t type_i = 0; type_i < element_count; type_i++) {
			type_max.write[type_i] = MAX(attribs[(i * element_count) + type_i], type_max[type_i]);
			type_min.write[type_i] = MIN(attribs[(i * element_count) + type_i], type_min[type_i]);
			type_max.write[type_i] = _filter_number(type_max.write[type_i]);
			type_min.write[type_i] = _filter_number(type_min.write[type_i]);
		}
	}

	ERR_FAIL_COND_V(attribs.size() == 0, -1);

	Ref<FBXAccessor> accessor;
	accessor.instantiate();
	FBXBufferIndex buffer_view_i;
	int64_t size = p_state->buffers[0].size();
	const FBXType type = FBXType::TYPE_SCALAR;
	const int component_type = FBXDocument::COMPONENT_TYPE_INT;

	accessor->max = type_max;
	accessor->min = type_min;
	accessor->normalized = false;
	accessor->count = ret_size;
	accessor->type = type;
	accessor->component_type = component_type;
	accessor->byte_offset = 0;
	Error err = _encode_buffer_view(p_state, attribs.ptr(), attribs.size(), type, component_type, accessor->normalized, size, p_for_vertex, buffer_view_i);
	if (err != OK) {
		return -1;
	}
	accessor->buffer_view = buffer_view_i;
	p_state->accessors.push_back(accessor);
	return p_state->accessors.size() - 1;
}

Vector<int> FBXDocument::_decode_accessor_as_ints(Ref<FBXState> p_state, const FBXAccessorIndex p_accessor, const bool p_for_vertex) {
	const Vector<double> attribs = _decode_accessor(p_state, p_accessor, p_for_vertex);
	Vector<int> ret;

	if (attribs.size() == 0) {
		return ret;
	}

	const double *attribs_ptr = attribs.ptr();
	const int ret_size = attribs.size();
	ret.resize(ret_size);
	{
		for (int i = 0; i < ret_size; i++) {
			ret.write[i] = int(attribs_ptr[i]);
		}
	}
	return ret;
}

Vector<float> FBXDocument::_decode_accessor_as_floats(Ref<FBXState> p_state, const FBXAccessorIndex p_accessor, const bool p_for_vertex) {
	const Vector<double> attribs = _decode_accessor(p_state, p_accessor, p_for_vertex);
	Vector<float> ret;

	if (attribs.size() == 0) {
		return ret;
	}

	const double *attribs_ptr = attribs.ptr();
	const int ret_size = attribs.size();
	ret.resize(ret_size);
	{
		for (int i = 0; i < ret_size; i++) {
			ret.write[i] = float(attribs_ptr[i]);
		}
	}
	return ret;
}

FBXAccessorIndex FBXDocument::_encode_accessor_as_vec2(Ref<FBXState> p_state, const Vector<Vector2> p_attribs, const bool p_for_vertex) {
	if (p_attribs.size() == 0) {
		return -1;
	}
	const int element_count = 2;

	const int ret_size = p_attribs.size() * element_count;
	Vector<double> attribs;
	attribs.resize(ret_size);
	Vector<double> type_max;
	type_max.resize(element_count);
	Vector<double> type_min;
	type_min.resize(element_count);

	for (int i = 0; i < p_attribs.size(); i++) {
		Vector2 attrib = p_attribs[i];
		attribs.write[(i * element_count) + 0] = Math::snapped(attrib.x, CMP_NORMALIZE_TOLERANCE);
		attribs.write[(i * element_count) + 1] = Math::snapped(attrib.y, CMP_NORMALIZE_TOLERANCE);
		_calc_accessor_min_max(i, element_count, type_max, attribs, type_min);
	}

	ERR_FAIL_COND_V(attribs.size() % element_count != 0, -1);

	Ref<FBXAccessor> accessor;
	accessor.instantiate();
	FBXBufferIndex buffer_view_i;
	int64_t size = p_state->buffers[0].size();
	const FBXType type = FBXType::TYPE_VEC2;
	const int component_type = FBXDocument::COMPONENT_TYPE_FLOAT;

	accessor->max = type_max;
	accessor->min = type_min;
	accessor->normalized = false;
	accessor->count = p_attribs.size();
	accessor->type = type;
	accessor->component_type = component_type;
	accessor->byte_offset = 0;
	Error err = _encode_buffer_view(p_state, attribs.ptr(), p_attribs.size(), type, component_type, accessor->normalized, size, p_for_vertex, buffer_view_i);
	if (err != OK) {
		return -1;
	}
	accessor->buffer_view = buffer_view_i;
	p_state->accessors.push_back(accessor);
	return p_state->accessors.size() - 1;
}

FBXAccessorIndex FBXDocument::_encode_accessor_as_color(Ref<FBXState> p_state, const Vector<Color> p_attribs, const bool p_for_vertex) {
	if (p_attribs.size() == 0) {
		return -1;
	}

	const int ret_size = p_attribs.size() * 4;
	Vector<double> attribs;
	attribs.resize(ret_size);

	const int element_count = 4;
	Vector<double> type_max;
	type_max.resize(element_count);
	Vector<double> type_min;
	type_min.resize(element_count);
	for (int i = 0; i < p_attribs.size(); i++) {
		Color attrib = p_attribs[i];
		attribs.write[(i * element_count) + 0] = Math::snapped(attrib.r, CMP_NORMALIZE_TOLERANCE);
		attribs.write[(i * element_count) + 1] = Math::snapped(attrib.g, CMP_NORMALIZE_TOLERANCE);
		attribs.write[(i * element_count) + 2] = Math::snapped(attrib.b, CMP_NORMALIZE_TOLERANCE);
		attribs.write[(i * element_count) + 3] = Math::snapped(attrib.a, CMP_NORMALIZE_TOLERANCE);

		_calc_accessor_min_max(i, element_count, type_max, attribs, type_min);
	}

	ERR_FAIL_COND_V(attribs.size() % element_count != 0, -1);

	Ref<FBXAccessor> accessor;
	accessor.instantiate();
	FBXBufferIndex buffer_view_i;
	int64_t size = p_state->buffers[0].size();
	const FBXType type = FBXType::TYPE_VEC4;
	const int component_type = FBXDocument::COMPONENT_TYPE_FLOAT;

	accessor->max = type_max;
	accessor->min = type_min;
	accessor->normalized = false;
	accessor->count = p_attribs.size();
	accessor->type = type;
	accessor->component_type = component_type;
	accessor->byte_offset = 0;
	Error err = _encode_buffer_view(p_state, attribs.ptr(), p_attribs.size(), type, component_type, accessor->normalized, size, p_for_vertex, buffer_view_i);
	if (err != OK) {
		return -1;
	}
	accessor->buffer_view = buffer_view_i;
	p_state->accessors.push_back(accessor);
	return p_state->accessors.size() - 1;
}

void FBXDocument::_calc_accessor_min_max(int p_i, const int p_element_count, Vector<double> &p_type_max, Vector<double> p_attribs, Vector<double> &p_type_min) {
	if (p_i == 0) {
		for (int32_t type_i = 0; type_i < p_element_count; type_i++) {
			p_type_max.write[type_i] = p_attribs[(p_i * p_element_count) + type_i];
			p_type_min.write[type_i] = p_attribs[(p_i * p_element_count) + type_i];
		}
	}
	for (int32_t type_i = 0; type_i < p_element_count; type_i++) {
		p_type_max.write[type_i] = MAX(p_attribs[(p_i * p_element_count) + type_i], p_type_max[type_i]);
		p_type_min.write[type_i] = MIN(p_attribs[(p_i * p_element_count) + type_i], p_type_min[type_i]);
		p_type_max.write[type_i] = _filter_number(p_type_max.write[type_i]);
		p_type_min.write[type_i] = _filter_number(p_type_min.write[type_i]);
	}
}

FBXAccessorIndex FBXDocument::_encode_accessor_as_weights(Ref<FBXState> p_state, const Vector<Color> p_attribs, const bool p_for_vertex) {
	if (p_attribs.size() == 0) {
		return -1;
	}

	const int ret_size = p_attribs.size() * 4;
	Vector<double> attribs;
	attribs.resize(ret_size);

	const int element_count = 4;

	Vector<double> type_max;
	type_max.resize(element_count);
	Vector<double> type_min;
	type_min.resize(element_count);
	for (int i = 0; i < p_attribs.size(); i++) {
		Color attrib = p_attribs[i];
		attribs.write[(i * element_count) + 0] = Math::snapped(attrib.r, CMP_NORMALIZE_TOLERANCE);
		attribs.write[(i * element_count) + 1] = Math::snapped(attrib.g, CMP_NORMALIZE_TOLERANCE);
		attribs.write[(i * element_count) + 2] = Math::snapped(attrib.b, CMP_NORMALIZE_TOLERANCE);
		attribs.write[(i * element_count) + 3] = Math::snapped(attrib.a, CMP_NORMALIZE_TOLERANCE);

		_calc_accessor_min_max(i, element_count, type_max, attribs, type_min);
	}

	ERR_FAIL_COND_V(attribs.size() % element_count != 0, -1);

	Ref<FBXAccessor> accessor;
	accessor.instantiate();
	FBXBufferIndex buffer_view_i;
	int64_t size = p_state->buffers[0].size();
	const FBXType type = FBXType::TYPE_VEC4;
	const int component_type = FBXDocument::COMPONENT_TYPE_FLOAT;

	accessor->max = type_max;
	accessor->min = type_min;
	accessor->normalized = false;
	accessor->count = p_attribs.size();
	accessor->type = type;
	accessor->component_type = component_type;
	accessor->byte_offset = 0;
	Error err = _encode_buffer_view(p_state, attribs.ptr(), p_attribs.size(), type, component_type, accessor->normalized, size, p_for_vertex, buffer_view_i);
	if (err != OK) {
		return -1;
	}
	accessor->buffer_view = buffer_view_i;
	p_state->accessors.push_back(accessor);
	return p_state->accessors.size() - 1;
}

FBXAccessorIndex FBXDocument::_encode_accessor_as_joints(Ref<FBXState> p_state, const Vector<Color> p_attribs, const bool p_for_vertex) {
	if (p_attribs.size() == 0) {
		return -1;
	}

	const int element_count = 4;
	const int ret_size = p_attribs.size() * element_count;
	Vector<double> attribs;
	attribs.resize(ret_size);

	Vector<double> type_max;
	type_max.resize(element_count);
	Vector<double> type_min;
	type_min.resize(element_count);
	for (int i = 0; i < p_attribs.size(); i++) {
		Color attrib = p_attribs[i];
		attribs.write[(i * element_count) + 0] = Math::snapped(attrib.r, CMP_NORMALIZE_TOLERANCE);
		attribs.write[(i * element_count) + 1] = Math::snapped(attrib.g, CMP_NORMALIZE_TOLERANCE);
		attribs.write[(i * element_count) + 2] = Math::snapped(attrib.b, CMP_NORMALIZE_TOLERANCE);
		attribs.write[(i * element_count) + 3] = Math::snapped(attrib.a, CMP_NORMALIZE_TOLERANCE);
		_calc_accessor_min_max(i, element_count, type_max, attribs, type_min);
	}
	ERR_FAIL_COND_V(attribs.size() % element_count != 0, -1);

	Ref<FBXAccessor> accessor;
	accessor.instantiate();
	FBXBufferIndex buffer_view_i;
	int64_t size = p_state->buffers[0].size();
	const FBXType type = FBXType::TYPE_VEC4;
	const int component_type = FBXDocument::COMPONENT_TYPE_UNSIGNED_SHORT;

	accessor->max = type_max;
	accessor->min = type_min;
	accessor->normalized = false;
	accessor->count = p_attribs.size();
	accessor->type = type;
	accessor->component_type = component_type;
	accessor->byte_offset = 0;
	Error err = _encode_buffer_view(p_state, attribs.ptr(), p_attribs.size(), type, component_type, accessor->normalized, size, p_for_vertex, buffer_view_i);
	if (err != OK) {
		return -1;
	}
	accessor->buffer_view = buffer_view_i;
	p_state->accessors.push_back(accessor);
	return p_state->accessors.size() - 1;
}

FBXAccessorIndex FBXDocument::_encode_accessor_as_quaternions(Ref<FBXState> p_state, const Vector<Quaternion> p_attribs, const bool p_for_vertex) {
	if (p_attribs.size() == 0) {
		return -1;
	}
	const int element_count = 4;

	const int ret_size = p_attribs.size() * element_count;
	Vector<double> attribs;
	attribs.resize(ret_size);

	Vector<double> type_max;
	type_max.resize(element_count);
	Vector<double> type_min;
	type_min.resize(element_count);
	for (int i = 0; i < p_attribs.size(); i++) {
		Quaternion quaternion = p_attribs[i];
		attribs.write[(i * element_count) + 0] = Math::snapped(quaternion.x, CMP_NORMALIZE_TOLERANCE);
		attribs.write[(i * element_count) + 1] = Math::snapped(quaternion.y, CMP_NORMALIZE_TOLERANCE);
		attribs.write[(i * element_count) + 2] = Math::snapped(quaternion.z, CMP_NORMALIZE_TOLERANCE);
		attribs.write[(i * element_count) + 3] = Math::snapped(quaternion.w, CMP_NORMALIZE_TOLERANCE);

		_calc_accessor_min_max(i, element_count, type_max, attribs, type_min);
	}

	ERR_FAIL_COND_V(attribs.size() % element_count != 0, -1);

	Ref<FBXAccessor> accessor;
	accessor.instantiate();
	FBXBufferIndex buffer_view_i;
	int64_t size = p_state->buffers[0].size();
	const FBXType type = FBXType::TYPE_VEC4;
	const int component_type = FBXDocument::COMPONENT_TYPE_FLOAT;

	accessor->max = type_max;
	accessor->min = type_min;
	accessor->normalized = false;
	accessor->count = p_attribs.size();
	accessor->type = type;
	accessor->component_type = component_type;
	accessor->byte_offset = 0;
	Error err = _encode_buffer_view(p_state, attribs.ptr(), p_attribs.size(), type, component_type, accessor->normalized, size, p_for_vertex, buffer_view_i);
	if (err != OK) {
		return -1;
	}
	accessor->buffer_view = buffer_view_i;
	p_state->accessors.push_back(accessor);
	return p_state->accessors.size() - 1;
}

Vector<Vector2> FBXDocument::_decode_accessor_as_vec2(Ref<FBXState> p_state, const FBXAccessorIndex p_accessor, const bool p_for_vertex) {
	const Vector<double> attribs = _decode_accessor(p_state, p_accessor, p_for_vertex);
	Vector<Vector2> ret;

	if (attribs.size() == 0) {
		return ret;
	}

	ERR_FAIL_COND_V(attribs.size() % 2 != 0, ret);
	const double *attribs_ptr = attribs.ptr();
	const int ret_size = attribs.size() / 2;
	ret.resize(ret_size);
	{
		for (int i = 0; i < ret_size; i++) {
			ret.write[i] = Vector2(attribs_ptr[i * 2 + 0], attribs_ptr[i * 2 + 1]);
		}
	}
	return ret;
}

FBXAccessorIndex FBXDocument::_encode_accessor_as_floats(Ref<FBXState> p_state, const Vector<real_t> p_attribs, const bool p_for_vertex) {
	if (p_attribs.size() == 0) {
		return -1;
	}
	const int element_count = 1;
	const int ret_size = p_attribs.size();
	Vector<double> attribs;
	attribs.resize(ret_size);

	Vector<double> type_max;
	type_max.resize(element_count);
	Vector<double> type_min;
	type_min.resize(element_count);

	for (int i = 0; i < p_attribs.size(); i++) {
		attribs.write[i] = Math::snapped(p_attribs[i], CMP_NORMALIZE_TOLERANCE);

		_calc_accessor_min_max(i, element_count, type_max, attribs, type_min);
	}

	ERR_FAIL_COND_V(!attribs.size(), -1);

	Ref<FBXAccessor> accessor;
	accessor.instantiate();
	FBXBufferIndex buffer_view_i;
	int64_t size = p_state->buffers[0].size();
	const FBXType type = FBXType::TYPE_SCALAR;
	const int component_type = FBXDocument::COMPONENT_TYPE_FLOAT;

	accessor->max = type_max;
	accessor->min = type_min;
	accessor->normalized = false;
	accessor->count = ret_size;
	accessor->type = type;
	accessor->component_type = component_type;
	accessor->byte_offset = 0;
	Error err = _encode_buffer_view(p_state, attribs.ptr(), attribs.size(), type, component_type, accessor->normalized, size, p_for_vertex, buffer_view_i);
	if (err != OK) {
		return -1;
	}
	accessor->buffer_view = buffer_view_i;
	p_state->accessors.push_back(accessor);
	return p_state->accessors.size() - 1;
}

FBXAccessorIndex FBXDocument::_encode_accessor_as_vec3(Ref<FBXState> p_state, const Vector<Vector3> p_attribs, const bool p_for_vertex) {
	if (p_attribs.size() == 0) {
		return -1;
	}
	const int element_count = 3;
	const int ret_size = p_attribs.size() * element_count;
	Vector<double> attribs;
	attribs.resize(ret_size);

	Vector<double> type_max;
	type_max.resize(element_count);
	Vector<double> type_min;
	type_min.resize(element_count);
	for (int i = 0; i < p_attribs.size(); i++) {
		Vector3 attrib = p_attribs[i];
		attribs.write[(i * element_count) + 0] = Math::snapped(attrib.x, CMP_NORMALIZE_TOLERANCE);
		attribs.write[(i * element_count) + 1] = Math::snapped(attrib.y, CMP_NORMALIZE_TOLERANCE);
		attribs.write[(i * element_count) + 2] = Math::snapped(attrib.z, CMP_NORMALIZE_TOLERANCE);

		_calc_accessor_min_max(i, element_count, type_max, attribs, type_min);
	}
	ERR_FAIL_COND_V(attribs.size() % element_count != 0, -1);

	Ref<FBXAccessor> accessor;
	accessor.instantiate();
	FBXBufferIndex buffer_view_i;
	int64_t size = p_state->buffers[0].size();
	const FBXType type = FBXType::TYPE_VEC3;
	const int component_type = FBXDocument::COMPONENT_TYPE_FLOAT;

	accessor->max = type_max;
	accessor->min = type_min;
	accessor->normalized = false;
	accessor->count = p_attribs.size();
	accessor->type = type;
	accessor->component_type = component_type;
	accessor->byte_offset = 0;
	Error err = _encode_buffer_view(p_state, attribs.ptr(), p_attribs.size(), type, component_type, accessor->normalized, size, p_for_vertex, buffer_view_i);
	if (err != OK) {
		return -1;
	}
	accessor->buffer_view = buffer_view_i;
	p_state->accessors.push_back(accessor);
	return p_state->accessors.size() - 1;
}

FBXAccessorIndex FBXDocument::_encode_accessor_as_xform(Ref<FBXState> p_state, const Vector<Transform3D> p_attribs, const bool p_for_vertex) {
	if (p_attribs.size() == 0) {
		return -1;
	}
	const int element_count = 16;
	const int ret_size = p_attribs.size() * element_count;
	Vector<double> attribs;
	attribs.resize(ret_size);

	Vector<double> type_max;
	type_max.resize(element_count);
	Vector<double> type_min;
	type_min.resize(element_count);
	for (int i = 0; i < p_attribs.size(); i++) {
		Transform3D attrib = p_attribs[i];
		Basis basis = attrib.get_basis();
		Vector3 axis_0 = basis.get_column(Vector3::AXIS_X);

		attribs.write[i * element_count + 0] = Math::snapped(axis_0.x, CMP_NORMALIZE_TOLERANCE);
		attribs.write[i * element_count + 1] = Math::snapped(axis_0.y, CMP_NORMALIZE_TOLERANCE);
		attribs.write[i * element_count + 2] = Math::snapped(axis_0.z, CMP_NORMALIZE_TOLERANCE);
		attribs.write[i * element_count + 3] = 0.0;

		Vector3 axis_1 = basis.get_column(Vector3::AXIS_Y);
		attribs.write[i * element_count + 4] = Math::snapped(axis_1.x, CMP_NORMALIZE_TOLERANCE);
		attribs.write[i * element_count + 5] = Math::snapped(axis_1.y, CMP_NORMALIZE_TOLERANCE);
		attribs.write[i * element_count + 6] = Math::snapped(axis_1.z, CMP_NORMALIZE_TOLERANCE);
		attribs.write[i * element_count + 7] = 0.0;

		Vector3 axis_2 = basis.get_column(Vector3::AXIS_Z);
		attribs.write[i * element_count + 8] = Math::snapped(axis_2.x, CMP_NORMALIZE_TOLERANCE);
		attribs.write[i * element_count + 9] = Math::snapped(axis_2.y, CMP_NORMALIZE_TOLERANCE);
		attribs.write[i * element_count + 10] = Math::snapped(axis_2.z, CMP_NORMALIZE_TOLERANCE);
		attribs.write[i * element_count + 11] = 0.0;

		Vector3 origin = attrib.get_origin();
		attribs.write[i * element_count + 12] = Math::snapped(origin.x, CMP_NORMALIZE_TOLERANCE);
		attribs.write[i * element_count + 13] = Math::snapped(origin.y, CMP_NORMALIZE_TOLERANCE);
		attribs.write[i * element_count + 14] = Math::snapped(origin.z, CMP_NORMALIZE_TOLERANCE);
		attribs.write[i * element_count + 15] = 1.0;

		_calc_accessor_min_max(i, element_count, type_max, attribs, type_min);
	}
	ERR_FAIL_COND_V(attribs.size() % element_count != 0, -1);

	Ref<FBXAccessor> accessor;
	accessor.instantiate();
	FBXBufferIndex buffer_view_i;
	int64_t size = p_state->buffers[0].size();
	const FBXType type = FBXType::TYPE_MAT4;
	const int component_type = FBXDocument::COMPONENT_TYPE_FLOAT;

	accessor->max = type_max;
	accessor->min = type_min;
	accessor->normalized = false;
	accessor->count = p_attribs.size();
	accessor->type = type;
	accessor->component_type = component_type;
	accessor->byte_offset = 0;
	Error err = _encode_buffer_view(p_state, attribs.ptr(), p_attribs.size(), type, component_type, accessor->normalized, size, p_for_vertex, buffer_view_i);
	if (err != OK) {
		return -1;
	}
	accessor->buffer_view = buffer_view_i;
	p_state->accessors.push_back(accessor);
	return p_state->accessors.size() - 1;
}

Vector<Vector3> FBXDocument::_decode_accessor_as_vec3(Ref<FBXState> p_state, const FBXAccessorIndex p_accessor, const bool p_for_vertex) {
	const Vector<double> attribs = _decode_accessor(p_state, p_accessor, p_for_vertex);
	Vector<Vector3> ret;

	if (attribs.size() == 0) {
		return ret;
	}

	ERR_FAIL_COND_V(attribs.size() % 3 != 0, ret);
	const double *attribs_ptr = attribs.ptr();
	const int ret_size = attribs.size() / 3;
	ret.resize(ret_size);
	{
		for (int i = 0; i < ret_size; i++) {
			ret.write[i] = Vector3(attribs_ptr[i * 3 + 0], attribs_ptr[i * 3 + 1], attribs_ptr[i * 3 + 2]);
		}
	}
	return ret;
}

Vector<Color> FBXDocument::_decode_accessor_as_color(Ref<FBXState> p_state, const FBXAccessorIndex p_accessor, const bool p_for_vertex) {
	const Vector<double> attribs = _decode_accessor(p_state, p_accessor, p_for_vertex);
	Vector<Color> ret;

	if (attribs.size() == 0) {
		return ret;
	}

	const int type = p_state->accessors[p_accessor]->type;
	ERR_FAIL_COND_V(!(type == TYPE_VEC3 || type == TYPE_VEC4), ret);
	int vec_len = 3;
	if (type == TYPE_VEC4) {
		vec_len = 4;
	}

	ERR_FAIL_COND_V(attribs.size() % vec_len != 0, ret);
	const double *attribs_ptr = attribs.ptr();
	const int ret_size = attribs.size() / vec_len;
	ret.resize(ret_size);
	{
		for (int i = 0; i < ret_size; i++) {
			ret.write[i] = Color(attribs_ptr[i * vec_len + 0], attribs_ptr[i * vec_len + 1], attribs_ptr[i * vec_len + 2], vec_len == 4 ? attribs_ptr[i * 4 + 3] : 1.0);
		}
	}
	return ret;
}
Vector<Quaternion> FBXDocument::_decode_accessor_as_quaternion(Ref<FBXState> p_state, const FBXAccessorIndex p_accessor, const bool p_for_vertex) {
	const Vector<double> attribs = _decode_accessor(p_state, p_accessor, p_for_vertex);
	Vector<Quaternion> ret;

	if (attribs.size() == 0) {
		return ret;
	}

	ERR_FAIL_COND_V(attribs.size() % 4 != 0, ret);
	const double *attribs_ptr = attribs.ptr();
	const int ret_size = attribs.size() / 4;
	ret.resize(ret_size);
	{
		for (int i = 0; i < ret_size; i++) {
			ret.write[i] = Quaternion(attribs_ptr[i * 4 + 0], attribs_ptr[i * 4 + 1], attribs_ptr[i * 4 + 2], attribs_ptr[i * 4 + 3]).normalized();
		}
	}
	return ret;
}
Vector<Transform2D> FBXDocument::_decode_accessor_as_xform2d(Ref<FBXState> p_state, const FBXAccessorIndex p_accessor, const bool p_for_vertex) {
	const Vector<double> attribs = _decode_accessor(p_state, p_accessor, p_for_vertex);
	Vector<Transform2D> ret;

	if (attribs.size() == 0) {
		return ret;
	}

	ERR_FAIL_COND_V(attribs.size() % 4 != 0, ret);
	ret.resize(attribs.size() / 4);
	for (int i = 0; i < ret.size(); i++) {
		ret.write[i][0] = Vector2(attribs[i * 4 + 0], attribs[i * 4 + 1]);
		ret.write[i][1] = Vector2(attribs[i * 4 + 2], attribs[i * 4 + 3]);
	}
	return ret;
}

Vector<Basis> FBXDocument::_decode_accessor_as_basis(Ref<FBXState> p_state, const FBXAccessorIndex p_accessor, const bool p_for_vertex) {
	const Vector<double> attribs = _decode_accessor(p_state, p_accessor, p_for_vertex);
	Vector<Basis> ret;

	if (attribs.size() == 0) {
		return ret;
	}

	ERR_FAIL_COND_V(attribs.size() % 9 != 0, ret);
	ret.resize(attribs.size() / 9);
	for (int i = 0; i < ret.size(); i++) {
		ret.write[i].set_column(0, Vector3(attribs[i * 9 + 0], attribs[i * 9 + 1], attribs[i * 9 + 2]));
		ret.write[i].set_column(1, Vector3(attribs[i * 9 + 3], attribs[i * 9 + 4], attribs[i * 9 + 5]));
		ret.write[i].set_column(2, Vector3(attribs[i * 9 + 6], attribs[i * 9 + 7], attribs[i * 9 + 8]));
	}
	return ret;
}

Vector<Transform3D> FBXDocument::_decode_accessor_as_xform(Ref<FBXState> p_state, const FBXAccessorIndex p_accessor, const bool p_for_vertex) {
	const Vector<double> attribs = _decode_accessor(p_state, p_accessor, p_for_vertex);
	Vector<Transform3D> ret;

	if (attribs.size() == 0) {
		return ret;
	}

	ERR_FAIL_COND_V(attribs.size() % 16 != 0, ret);
	ret.resize(attribs.size() / 16);
	for (int i = 0; i < ret.size(); i++) {
		ret.write[i].basis.set_column(0, Vector3(attribs[i * 16 + 0], attribs[i * 16 + 1], attribs[i * 16 + 2]));
		ret.write[i].basis.set_column(1, Vector3(attribs[i * 16 + 4], attribs[i * 16 + 5], attribs[i * 16 + 6]));
		ret.write[i].basis.set_column(2, Vector3(attribs[i * 16 + 8], attribs[i * 16 + 9], attribs[i * 16 + 10]));
		ret.write[i].set_origin(Vector3(attribs[i * 16 + 12], attribs[i * 16 + 13], attribs[i * 16 + 14]));
	}
	return ret;
}

Error FBXDocument::_parse_meshes(Ref<FBXState> p_state) {
	if (!p_state->json.has("meshes")) {
		return OK;
	}

	Array meshes = p_state->json["meshes"];
	for (FBXMeshIndex i = 0; i < meshes.size(); i++) {
		print_verbose("glTF: Parsing mesh: " + itos(i));
		Dictionary d = meshes[i];

		Ref<FBXMesh> mesh;
		mesh.instantiate();
		bool has_vertex_color = false;

		ERR_FAIL_COND_V(!d.has("primitives"), ERR_PARSE_ERROR);

		Array primitives = d["primitives"];
		const Dictionary &extras = d.has("extras") ? (Dictionary)d["extras"] : Dictionary();
		Ref<ImporterMesh> import_mesh;
		import_mesh.instantiate();
		String mesh_name = "mesh";
		if (d.has("name") && !String(d["name"]).is_empty()) {
			mesh_name = d["name"];
		}
		import_mesh->set_name(_gen_unique_name(p_state, vformat("%s_%s", p_state->scene_name, mesh_name)));

		for (int j = 0; j < primitives.size(); j++) {
			uint32_t flags = 0;
			Dictionary p = primitives[j];

			Array array;
			array.resize(Mesh::ARRAY_MAX);

			ERR_FAIL_COND_V(!p.has("attributes"), ERR_PARSE_ERROR);

			Dictionary a = p["attributes"];

			Mesh::PrimitiveType primitive = Mesh::PRIMITIVE_TRIANGLES;
			if (p.has("mode")) {
				const int mode = p["mode"];
				ERR_FAIL_INDEX_V(mode, 7, ERR_FILE_CORRUPT);
				// Convert mesh.primitive.mode to Godot Mesh enum. See:
				// https://www.khronos.org/registry/glTF/specs/2.0/glTF-2.0.html#_mesh_primitive_mode
				static const Mesh::PrimitiveType primitives2[7] = {
					Mesh::PRIMITIVE_POINTS, // 0 POINTS
					Mesh::PRIMITIVE_LINES, // 1 LINES
					Mesh::PRIMITIVE_LINES, // 2 LINE_LOOP; loop not supported, should be converted
					Mesh::PRIMITIVE_LINE_STRIP, // 3 LINE_STRIP
					Mesh::PRIMITIVE_TRIANGLES, // 4 TRIANGLES
					Mesh::PRIMITIVE_TRIANGLE_STRIP, // 5 TRIANGLE_STRIP
					Mesh::PRIMITIVE_TRIANGLES, // 6 TRIANGLE_FAN fan not supported, should be converted
					// TODO: Line loop and triangle fan are not supported and need to be converted to lines and triangles.
				};

				primitive = primitives2[mode];
			}

			ERR_FAIL_COND_V(!a.has("POSITION"), ERR_PARSE_ERROR);
			int32_t vertex_num = 0;
			if (a.has("POSITION")) {
				PackedVector3Array vertices = _decode_accessor_as_vec3(p_state, a["POSITION"], true);
				array[Mesh::ARRAY_VERTEX] = vertices;
				vertex_num = vertices.size();
			}
			if (a.has("NORMAL")) {
				array[Mesh::ARRAY_NORMAL] = _decode_accessor_as_vec3(p_state, a["NORMAL"], true);
			}
			if (a.has("TANGENT")) {
				array[Mesh::ARRAY_TANGENT] = _decode_accessor_as_floats(p_state, a["TANGENT"], true);
			}
			if (a.has("TEXCOORD_0")) {
				array[Mesh::ARRAY_TEX_UV] = _decode_accessor_as_vec2(p_state, a["TEXCOORD_0"], true);
			}
			if (a.has("TEXCOORD_1")) {
				array[Mesh::ARRAY_TEX_UV2] = _decode_accessor_as_vec2(p_state, a["TEXCOORD_1"], true);
			}
			for (int custom_i = 0; custom_i < 3; custom_i++) {
				Vector<float> cur_custom;
				Vector<Vector2> texcoord_first;
				Vector<Vector2> texcoord_second;

				int texcoord_i = 2 + 2 * custom_i;
				String gltf_texcoord_key = vformat("TEXCOORD_%d", texcoord_i);
				int num_channels = 0;
				if (a.has(gltf_texcoord_key)) {
					texcoord_first = _decode_accessor_as_vec2(p_state, a[gltf_texcoord_key], true);
					num_channels = 2;
				}
				gltf_texcoord_key = vformat("TEXCOORD_%d", texcoord_i + 1);
				if (a.has(gltf_texcoord_key)) {
					texcoord_second = _decode_accessor_as_vec2(p_state, a[gltf_texcoord_key], true);
					num_channels = 4;
				}
				if (!num_channels) {
					break;
				}
				if (num_channels == 2 || num_channels == 4) {
					cur_custom.resize(vertex_num * num_channels);
					for (int32_t uv_i = 0; uv_i < texcoord_first.size() && uv_i < vertex_num; uv_i++) {
						cur_custom.write[uv_i * num_channels + 0] = texcoord_first[uv_i].x;
						cur_custom.write[uv_i * num_channels + 1] = texcoord_first[uv_i].y;
					}
					// Vector.resize seems to not zero-initialize. Ensure all unused elements are 0:
					for (int32_t uv_i = texcoord_first.size(); uv_i < vertex_num; uv_i++) {
						cur_custom.write[uv_i * num_channels + 0] = 0;
						cur_custom.write[uv_i * num_channels + 1] = 0;
					}
				}
				if (num_channels == 4) {
					for (int32_t uv_i = 0; uv_i < texcoord_second.size() && uv_i < vertex_num; uv_i++) {
						// num_channels must be 4
						cur_custom.write[uv_i * num_channels + 2] = texcoord_second[uv_i].x;
						cur_custom.write[uv_i * num_channels + 3] = texcoord_second[uv_i].y;
					}
					// Vector.resize seems to not zero-initialize. Ensure all unused elements are 0:
					for (int32_t uv_i = texcoord_second.size(); uv_i < vertex_num; uv_i++) {
						cur_custom.write[uv_i * num_channels + 2] = 0;
						cur_custom.write[uv_i * num_channels + 3] = 0;
					}
				}
				if (cur_custom.size() > 0) {
					array[Mesh::ARRAY_CUSTOM0 + custom_i] = cur_custom;
					int custom_shift = Mesh::ARRAY_FORMAT_CUSTOM0_SHIFT + custom_i * Mesh::ARRAY_FORMAT_CUSTOM_BITS;
					if (num_channels == 2) {
						flags |= Mesh::ARRAY_CUSTOM_RG_FLOAT << custom_shift;
					} else {
						flags |= Mesh::ARRAY_CUSTOM_RGBA_FLOAT << custom_shift;
					}
				}
			}
			if (a.has("COLOR_0")) {
				array[Mesh::ARRAY_COLOR] = _decode_accessor_as_color(p_state, a["COLOR_0"], true);
				has_vertex_color = true;
			}
			if (a.has("JOINTS_0") && !a.has("JOINTS_1")) {
				array[Mesh::ARRAY_BONES] = _decode_accessor_as_ints(p_state, a["JOINTS_0"], true);
			} else if (a.has("JOINTS_0") && a.has("JOINTS_1")) {
				PackedInt32Array joints_0 = _decode_accessor_as_ints(p_state, a["JOINTS_0"], true);
				PackedInt32Array joints_1 = _decode_accessor_as_ints(p_state, a["JOINTS_1"], true);
				ERR_FAIL_COND_V(joints_0.size() != joints_1.size(), ERR_INVALID_DATA);
				int32_t weight_8_count = JOINT_GROUP_SIZE * 2;
				Vector<int> joints;
				joints.resize(vertex_num * weight_8_count);
				for (int32_t vertex_i = 0; vertex_i < vertex_num; vertex_i++) {
					joints.write[vertex_i * weight_8_count + 0] = joints_0[vertex_i * JOINT_GROUP_SIZE + 0];
					joints.write[vertex_i * weight_8_count + 1] = joints_0[vertex_i * JOINT_GROUP_SIZE + 1];
					joints.write[vertex_i * weight_8_count + 2] = joints_0[vertex_i * JOINT_GROUP_SIZE + 2];
					joints.write[vertex_i * weight_8_count + 3] = joints_0[vertex_i * JOINT_GROUP_SIZE + 3];
					joints.write[vertex_i * weight_8_count + 4] = joints_1[vertex_i * JOINT_GROUP_SIZE + 0];
					joints.write[vertex_i * weight_8_count + 5] = joints_1[vertex_i * JOINT_GROUP_SIZE + 1];
					joints.write[vertex_i * weight_8_count + 6] = joints_1[vertex_i * JOINT_GROUP_SIZE + 2];
					joints.write[vertex_i * weight_8_count + 7] = joints_1[vertex_i * JOINT_GROUP_SIZE + 3];
				}
				array[Mesh::ARRAY_BONES] = joints;
			}
			if (a.has("WEIGHTS_0") && !a.has("WEIGHTS_1")) {
				Vector<float> weights = _decode_accessor_as_floats(p_state, a["WEIGHTS_0"], true);
				{ //gltf does not seem to normalize the weights for some reason..
					int wc = weights.size();
					float *w = weights.ptrw();

					for (int k = 0; k < wc; k += 4) {
						float total = 0.0;
						total += w[k + 0];
						total += w[k + 1];
						total += w[k + 2];
						total += w[k + 3];
						if (total > 0.0) {
							w[k + 0] /= total;
							w[k + 1] /= total;
							w[k + 2] /= total;
							w[k + 3] /= total;
						}
					}
				}
				array[Mesh::ARRAY_WEIGHTS] = weights;
			} else if (a.has("WEIGHTS_0") && a.has("WEIGHTS_1")) {
				Vector<float> weights_0 = _decode_accessor_as_floats(p_state, a["WEIGHTS_0"], true);
				Vector<float> weights_1 = _decode_accessor_as_floats(p_state, a["WEIGHTS_1"], true);
				Vector<float> weights;
				ERR_FAIL_COND_V(weights_0.size() != weights_1.size(), ERR_INVALID_DATA);
				int32_t weight_8_count = JOINT_GROUP_SIZE * 2;
				weights.resize(vertex_num * weight_8_count);
				for (int32_t vertex_i = 0; vertex_i < vertex_num; vertex_i++) {
					weights.write[vertex_i * weight_8_count + 0] = weights_0[vertex_i * JOINT_GROUP_SIZE + 0];
					weights.write[vertex_i * weight_8_count + 1] = weights_0[vertex_i * JOINT_GROUP_SIZE + 1];
					weights.write[vertex_i * weight_8_count + 2] = weights_0[vertex_i * JOINT_GROUP_SIZE + 2];
					weights.write[vertex_i * weight_8_count + 3] = weights_0[vertex_i * JOINT_GROUP_SIZE + 3];
					weights.write[vertex_i * weight_8_count + 4] = weights_1[vertex_i * JOINT_GROUP_SIZE + 0];
					weights.write[vertex_i * weight_8_count + 5] = weights_1[vertex_i * JOINT_GROUP_SIZE + 1];
					weights.write[vertex_i * weight_8_count + 6] = weights_1[vertex_i * JOINT_GROUP_SIZE + 2];
					weights.write[vertex_i * weight_8_count + 7] = weights_1[vertex_i * JOINT_GROUP_SIZE + 3];
				}
				{ //gltf does not seem to normalize the weights for some reason..
					int wc = weights.size();
					float *w = weights.ptrw();

					for (int k = 0; k < wc; k += weight_8_count) {
						float total = 0.0;
						total += w[k + 0];
						total += w[k + 1];
						total += w[k + 2];
						total += w[k + 3];
						total += w[k + 4];
						total += w[k + 5];
						total += w[k + 6];
						total += w[k + 7];
						if (total > 0.0) {
							w[k + 0] /= total;
							w[k + 1] /= total;
							w[k + 2] /= total;
							w[k + 3] /= total;
							w[k + 4] /= total;
							w[k + 5] /= total;
							w[k + 6] /= total;
							w[k + 7] /= total;
						}
					}
				}
				array[Mesh::ARRAY_WEIGHTS] = weights;
			}

			if (p.has("indices")) {
				Vector<int> indices = _decode_accessor_as_ints(p_state, p["indices"], false);

				if (primitive == Mesh::PRIMITIVE_TRIANGLES) {
					//swap around indices, convert ccw to cw for front face

					const int is = indices.size();
					int *w = indices.ptrw();
					for (int k = 0; k < is; k += 3) {
						SWAP(w[k + 1], w[k + 2]);
					}
				}
				array[Mesh::ARRAY_INDEX] = indices;

			} else if (primitive == Mesh::PRIMITIVE_TRIANGLES) {
				//generate indices because they need to be swapped for CW/CCW
				const Vector<Vector3> &vertices = array[Mesh::ARRAY_VERTEX];
				ERR_FAIL_COND_V(vertices.size() == 0, ERR_PARSE_ERROR);
				Vector<int> indices;
				const int vs = vertices.size();
				indices.resize(vs);
				{
					int *w = indices.ptrw();
					for (int k = 0; k < vs; k += 3) {
						w[k] = k;
						w[k + 1] = k + 2;
						w[k + 2] = k + 1;
					}
				}
				array[Mesh::ARRAY_INDEX] = indices;
			}

			bool generate_tangents = (primitive == Mesh::PRIMITIVE_TRIANGLES && !a.has("TANGENT") && a.has("TEXCOORD_0") && a.has("NORMAL"));

			Ref<SurfaceTool> mesh_surface_tool;
			mesh_surface_tool.instantiate();
			mesh_surface_tool->create_from_triangle_arrays(array);
			if (a.has("JOINTS_0") && a.has("JOINTS_1")) {
				mesh_surface_tool->set_skin_weight_count(SurfaceTool::SKIN_8_WEIGHTS);
			}
			mesh_surface_tool->index();
			if (generate_tangents) {
				//must generate mikktspace tangents.. ergh..
				mesh_surface_tool->generate_tangents();
			}
			array = mesh_surface_tool->commit_to_arrays();

			Array morphs;
			//blend shapes
			if (p.has("targets")) {
				print_verbose("glTF: Mesh has targets");
				const Array &targets = p["targets"];

				//ideally BLEND_SHAPE_MODE_RELATIVE since gltf2 stores in displacement
				//but it could require a larger refactor?
				import_mesh->set_blend_shape_mode(Mesh::BLEND_SHAPE_MODE_NORMALIZED);

				if (j == 0) {
					const Array &target_names = extras.has("targetNames") ? (Array)extras["targetNames"] : Array();
					for (int k = 0; k < targets.size(); k++) {
						String bs_name;
						if (k < target_names.size() && ((String)target_names[k]).size() != 0) {
							bs_name = (String)target_names[k];
						} else {
							bs_name = String("morph_") + itos(k);
						}
						import_mesh->add_blend_shape(bs_name);
					}
				}

				for (int k = 0; k < targets.size(); k++) {
					const Dictionary &t = targets[k];

					Array array_copy;
					array_copy.resize(Mesh::ARRAY_MAX);

					for (int l = 0; l < Mesh::ARRAY_MAX; l++) {
						array_copy[l] = array[l];
					}

					if (t.has("POSITION")) {
						Vector<Vector3> varr = _decode_accessor_as_vec3(p_state, t["POSITION"], true);
						const Vector<Vector3> src_varr = array[Mesh::ARRAY_VERTEX];
						const int size = src_varr.size();
						ERR_FAIL_COND_V(size == 0, ERR_PARSE_ERROR);
						{
							const int max_idx = varr.size();
							varr.resize(size);

							Vector3 *w_varr = varr.ptrw();
							const Vector3 *r_varr = varr.ptr();
							const Vector3 *r_src_varr = src_varr.ptr();
							for (int l = 0; l < size; l++) {
								if (l < max_idx) {
									w_varr[l] = r_varr[l] + r_src_varr[l];
								} else {
									w_varr[l] = r_src_varr[l];
								}
							}
						}
						array_copy[Mesh::ARRAY_VERTEX] = varr;
					}
					if (t.has("NORMAL")) {
						Vector<Vector3> narr = _decode_accessor_as_vec3(p_state, t["NORMAL"], true);
						const Vector<Vector3> src_narr = array[Mesh::ARRAY_NORMAL];
						int size = src_narr.size();
						ERR_FAIL_COND_V(size == 0, ERR_PARSE_ERROR);
						{
							int max_idx = narr.size();
							narr.resize(size);

							Vector3 *w_narr = narr.ptrw();
							const Vector3 *r_narr = narr.ptr();
							const Vector3 *r_src_narr = src_narr.ptr();
							for (int l = 0; l < size; l++) {
								if (l < max_idx) {
									w_narr[l] = r_narr[l] + r_src_narr[l];
								} else {
									w_narr[l] = r_src_narr[l];
								}
							}
						}
						array_copy[Mesh::ARRAY_NORMAL] = narr;
					}
					if (t.has("TANGENT")) {
						const Vector<Vector3> tangents_v3 = _decode_accessor_as_vec3(p_state, t["TANGENT"], true);
						const Vector<float> src_tangents = array[Mesh::ARRAY_TANGENT];
						ERR_FAIL_COND_V(src_tangents.size() == 0, ERR_PARSE_ERROR);

						Vector<float> tangents_v4;

						{
							int max_idx = tangents_v3.size();

							int size4 = src_tangents.size();
							tangents_v4.resize(size4);
							float *w4 = tangents_v4.ptrw();

							const Vector3 *r3 = tangents_v3.ptr();
							const float *r4 = src_tangents.ptr();

							for (int l = 0; l < size4 / 4; l++) {
								if (l < max_idx) {
									w4[l * 4 + 0] = r3[l].x + r4[l * 4 + 0];
									w4[l * 4 + 1] = r3[l].y + r4[l * 4 + 1];
									w4[l * 4 + 2] = r3[l].z + r4[l * 4 + 2];
								} else {
									w4[l * 4 + 0] = r4[l * 4 + 0];
									w4[l * 4 + 1] = r4[l * 4 + 1];
									w4[l * 4 + 2] = r4[l * 4 + 2];
								}
								w4[l * 4 + 3] = r4[l * 4 + 3]; //copy flip value
							}
						}

						array_copy[Mesh::ARRAY_TANGENT] = tangents_v4;
					}

					Ref<SurfaceTool> blend_surface_tool;
					blend_surface_tool.instantiate();
					blend_surface_tool->create_from_triangle_arrays(array_copy);
					if (a.has("JOINTS_0") && a.has("JOINTS_1")) {
						blend_surface_tool->set_skin_weight_count(SurfaceTool::SKIN_8_WEIGHTS);
					}
					blend_surface_tool->index();
					if (generate_tangents) {
						blend_surface_tool->generate_tangents();
					}
					array_copy = blend_surface_tool->commit_to_arrays();

					// Enforce blend shape mask array format
					for (int l = 0; l < Mesh::ARRAY_MAX; l++) {
						if (!(Mesh::ARRAY_FORMAT_BLEND_SHAPE_MASK & (1 << l))) {
							array_copy[l] = Variant();
						}
					}

					morphs.push_back(array_copy);
				}
			}

			Ref<Material> mat;
			String mat_name;
			if (!p_state->discard_meshes_and_materials) {
				if (p.has("material")) {
					const int material = p["material"];
					ERR_FAIL_INDEX_V(material, p_state->materials.size(), ERR_FILE_CORRUPT);
					Ref<Material> mat3d = p_state->materials[material];
					ERR_FAIL_NULL_V(mat3d, ERR_FILE_CORRUPT);

					Ref<BaseMaterial3D> base_material = mat3d;
					if (has_vertex_color && base_material.is_valid()) {
						base_material->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
					}
					mat = mat3d;

				} else {
					Ref<StandardMaterial3D> mat3d;
					mat3d.instantiate();
					if (has_vertex_color) {
						mat3d->set_flag(StandardMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
					}
					mat = mat3d;
				}
				ERR_FAIL_NULL_V(mat, ERR_FILE_CORRUPT);
				mat_name = mat->get_name();
			}
			import_mesh->add_surface(primitive, array, morphs,
					Dictionary(), mat, mat_name, flags);
		}

		Vector<float> blend_weights;
		blend_weights.resize(import_mesh->get_blend_shape_count());
		for (int32_t weight_i = 0; weight_i < blend_weights.size(); weight_i++) {
			blend_weights.write[weight_i] = 0.0f;
		}

		if (d.has("weights")) {
			const Array &weights = d["weights"];
			for (int j = 0; j < weights.size(); j++) {
				if (j >= blend_weights.size()) {
					break;
				}
				blend_weights.write[j] = weights[j];
			}
		}
		mesh->set_blend_weights(blend_weights);
		mesh->set_mesh(import_mesh);

		p_state->meshes.push_back(mesh);
	}

	print_verbose("glTF: Total meshes: " + itos(p_state->meshes.size()));

	return OK;
}

Ref<Image> FBXDocument::_parse_image_bytes_into_image(Ref<FBXState> p_state, const Vector<uint8_t> &p_bytes, const String &p_mime_type, int p_index, String &r_file_extension) {
	Ref<Image> r_image;
	r_image.instantiate();
	// Check if any FBXDocumentExtensions want to import this data as an image.
	for (Ref<FBXDocumentExtension> ext : document_extensions) {
		ERR_CONTINUE(ext.is_null());
		Error err = ext->parse_image_data(p_state, p_bytes, p_mime_type, r_image);
		ERR_CONTINUE_MSG(err != OK, "GLTF: Encountered error " + itos(err) + " when parsing image " + itos(p_index) + " in file " + p_state->filename + ". Continuing.");
		if (!r_image->is_empty()) {
			r_file_extension = ext->get_image_file_extension();
			return r_image;
		}
	}
	// If no extension wanted to import this data as an image, try to load a PNG or JPEG.
	// First we honor the mime types if they were defined.
	if (p_mime_type == "image/png") { // Load buffer as PNG.
		r_image->load_png_from_buffer(p_bytes);
		r_file_extension = ".png";
	} else if (p_mime_type == "image/jpeg") { // Loader buffer as JPEG.
		r_image->load_jpg_from_buffer(p_bytes);
		r_file_extension = ".jpg";
	}
	// If we didn't pass the above tests, we attempt loading as PNG and then JPEG directly.
	// This covers URIs with base64-encoded data with application/* type but
	// no optional mimeType property, or bufferViews with a bogus mimeType
	// (e.g. `image/jpeg` but the data is actually PNG).
	// That's not *exactly* what the spec mandates but this lets us be
	// lenient with bogus glb files which do exist in production.
	if (r_image->is_empty()) { // Try PNG first.
		r_image->load_png_from_buffer(p_bytes);
	}
	if (r_image->is_empty()) { // And then JPEG.
		r_image->load_jpg_from_buffer(p_bytes);
	}
	// If it still can't be loaded, give up and insert an empty image as placeholder.
	if (r_image->is_empty()) {
		ERR_PRINT(vformat("glTF: Couldn't load image index '%d' with its given mimetype: %s.", p_index, p_mime_type));
	}
	return r_image;
}

void FBXDocument::_parse_image_save_image(Ref<FBXState> p_state, const Vector<uint8_t> &p_bytes, const String &p_file_extension, int p_index, Ref<Image> p_image) {
	FBXState::GLTFHandleBinary handling = FBXState::GLTFHandleBinary(p_state->handle_binary_image);
	if (p_image->is_empty() || handling == FBXState::GLTFHandleBinary::HANDLE_BINARY_DISCARD_TEXTURES) {
		p_state->images.push_back(Ref<Texture2D>());
		p_state->source_images.push_back(Ref<Image>());
		return;
	}
#ifdef TOOLS_ENABLED
	if (Engine::get_singleton()->is_editor_hint() && handling == FBXState::GLTFHandleBinary::HANDLE_BINARY_EXTRACT_TEXTURES) {
		if (p_state->base_path.is_empty()) {
			p_state->images.push_back(Ref<Texture2D>());
			p_state->source_images.push_back(Ref<Image>());
		} else if (p_image->get_name().is_empty()) {
			WARN_PRINT(vformat("glTF: Image index '%d' couldn't be named. Skipping it.", p_index));
			p_state->images.push_back(Ref<Texture2D>());
			p_state->source_images.push_back(Ref<Image>());
		} else {
			bool must_import = true;
			Vector<uint8_t> img_data = p_image->get_data();
			Dictionary generator_parameters;
			String file_path = p_state->get_base_path() + "/" + p_state->filename.get_basename() + "_" + p_image->get_name();
			file_path += p_file_extension.is_empty() ? ".png" : p_file_extension;
			if (FileAccess::exists(file_path + ".import")) {
				Ref<ConfigFile> config;
				config.instantiate();
				config->load(file_path + ".import");
				if (config->has_section_key("remap", "generator_parameters")) {
					generator_parameters = (Dictionary)config->get_value("remap", "generator_parameters");
				}
				if (!generator_parameters.has("md5")) {
					must_import = false; // Didn't come from a gltf document; don't overwrite.
				}
				String existing_md5 = generator_parameters["md5"];
				unsigned char md5_hash[16];
				CryptoCore::md5(img_data.ptr(), img_data.size(), md5_hash);
				String new_md5 = String::hex_encode_buffer(md5_hash, 16);
				generator_parameters["md5"] = new_md5;
				if (new_md5 == existing_md5) {
					must_import = false;
				}
			}
			if (must_import) {
				Error err = OK;
				if (p_file_extension.is_empty()) {
					// If a file extension was not specified, save the image data to a PNG file.
					err = p_image->save_png(file_path);
					ERR_FAIL_COND(err != OK);
				} else {
					// If a file extension was specified, save the original bytes to a file with that extension.
					Ref<FileAccess> file = FileAccess::open(file_path, FileAccess::WRITE, &err);
					ERR_FAIL_COND(err != OK);
					file->store_buffer(p_bytes);
					file->close();
				}
				// ResourceLoader::import will crash if not is_editor_hint(), so this case is protected above and will fall through to uncompressed.
				HashMap<StringName, Variant> custom_options;
				custom_options[SNAME("mipmaps/generate")] = true;
				// Will only use project settings defaults if custom_importer is empty.
				EditorFileSystem::get_singleton()->update_file(file_path);
				EditorFileSystem::get_singleton()->reimport_append(file_path, custom_options, String(), generator_parameters);
			}
			Ref<Texture2D> saved_image = ResourceLoader::load(file_path, "Texture2D");
			if (saved_image.is_valid()) {
				p_state->images.push_back(saved_image);
				p_state->source_images.push_back(saved_image->get_image());
			} else {
				WARN_PRINT(vformat("glTF: Image index '%d' couldn't be loaded with the name: %s. Skipping it.", p_index, p_image->get_name()));
				// Placeholder to keep count.
				p_state->images.push_back(Ref<Texture2D>());
				p_state->source_images.push_back(Ref<Image>());
			}
		}
		return;
	}
#endif // TOOLS_ENABLED
	if (handling == FBXState::GLTFHandleBinary::HANDLE_BINARY_EMBED_AS_BASISU) {
		Ref<PortableCompressedTexture2D> tex;
		tex.instantiate();
		tex->set_name(p_image->get_name());
		tex->set_keep_compressed_buffer(true);
		tex->create_from_image(p_image, PortableCompressedTexture2D::COMPRESSION_MODE_BASIS_UNIVERSAL);
		p_state->images.push_back(tex);
		p_state->source_images.push_back(p_image);
		return;
	}
	// This handles the case of HANDLE_BINARY_EMBED_AS_UNCOMPRESSED, and it also serves
	// as a fallback for HANDLE_BINARY_EXTRACT_TEXTURES when this is not the editor.
	Ref<ImageTexture> tex;
	tex.instantiate();
	tex->set_name(p_image->get_name());
	tex->set_image(p_image);
	p_state->images.push_back(tex);
	p_state->source_images.push_back(p_image);
}

Error FBXDocument::_parse_images(Ref<FBXState> p_state, const String &p_base_path) {
	ERR_FAIL_NULL_V(p_state, ERR_INVALID_PARAMETER);
	if (!p_state->json.has("images")) {
		return OK;
	}

	// Ref: https://github.com/KhronosGroup/glTF/blob/master/specification/2.0/README.md#images

	const Array &images = p_state->json["images"];
	HashSet<String> used_names;
	for (int i = 0; i < images.size(); i++) {
		const Dictionary &dict = images[i];

		// glTF 2.0 supports PNG and JPEG types, which can be specified as (from spec):
		// "- a URI to an external file in one of the supported images formats, or
		//  - a URI with embedded base64-encoded data, or
		//  - a reference to a bufferView; in that case mimeType must be defined."
		// Since mimeType is optional for external files and base64 data, we'll have to
		// fall back on letting Godot parse the data to figure out if it's PNG or JPEG.

		// We'll assume that we use either URI or bufferView, so let's warn the user
		// if their image somehow uses both. And fail if it has neither.
		ERR_CONTINUE_MSG(!dict.has("uri") && !dict.has("bufferView"), "Invalid image definition in glTF file, it should specify an 'uri' or 'bufferView'.");
		if (dict.has("uri") && dict.has("bufferView")) {
			WARN_PRINT("Invalid image definition in glTF file using both 'uri' and 'bufferView'. 'uri' will take precedence.");
		}

		String mime_type;
		if (dict.has("mimeType")) { // Should be "image/png", "image/jpeg", or something handled by an extension.
			mime_type = dict["mimeType"];
		}

		String image_name;
		if (dict.has("name")) {
			image_name = dict["name"];
			image_name = image_name.get_file().get_basename().validate_filename();
		}
		if (image_name.is_empty()) {
			image_name = itos(i);
		}
		while (used_names.has(image_name)) {
			image_name += "_" + itos(i);
		}
		used_names.insert(image_name);
		// Load the image data. If we get a byte array, store here for later.
		Vector<uint8_t> data;
		if (dict.has("uri")) {
			// Handles the first two bullet points from the spec (embedded data, or external file).
			String uri = dict["uri"];
			if (uri.begins_with("data:")) { // Embedded data using base64.
				data = _parse_base64_uri(uri);
				// mimeType is optional, but if we have it defined in the URI, let's use it.
				if (mime_type.is_empty() && uri.contains(";")) {
					// Trim "data:" prefix which is 5 characters long, and end at ";base64".
					mime_type = uri.substr(5, uri.find(";base64") - 5);
				}
			} else { // Relative path to an external image file.
				ERR_FAIL_COND_V(p_base_path.is_empty(), ERR_INVALID_PARAMETER);
				uri = uri.uri_decode();
				uri = p_base_path.path_join(uri).replace("\\", "/"); // Fix for Windows.
				// ResourceLoader will rely on the file extension to use the relevant loader.
				// The spec says that if mimeType is defined, it should take precedence (e.g.
				// there could be a `.png` image which is actually JPEG), but there's no easy
				// API for that in Godot, so we'd have to load as a buffer (i.e. embedded in
				// the material), so we only do that only as fallback.
				Ref<Texture2D> texture = ResourceLoader::load(uri);
				if (texture.is_valid()) {
					p_state->images.push_back(texture);
					p_state->source_images.push_back(texture->get_image());
					continue;
				}
				// mimeType is optional, but if we have it in the file extension, let's use it.
				// If the mimeType does not match with the file extension, either it should be
				// specified in the file, or the FBXDocumentExtension should handle it.
				if (mime_type.is_empty()) {
					mime_type = "image/" + uri.get_extension();
				}
				// Fallback to loading as byte array. This enables us to support the
				// spec's requirement that we honor mimetype regardless of file URI.
				data = FileAccess::get_file_as_bytes(uri);
				if (data.size() == 0) {
					WARN_PRINT(vformat("glTF: Image index '%d' couldn't be loaded as a buffer of MIME type '%s' from URI: %s because there was no data to load. Skipping it.", i, mime_type, uri));
					p_state->images.push_back(Ref<Texture2D>()); // Placeholder to keep count.
					p_state->source_images.push_back(Ref<Image>());
					continue;
				}
			}
		} else if (dict.has("bufferView")) {
			// Handles the third bullet point from the spec (bufferView).
			ERR_FAIL_COND_V_MSG(mime_type.is_empty(), ERR_FILE_CORRUPT, vformat("glTF: Image index '%d' specifies 'bufferView' but no 'mimeType', which is invalid.", i));
			const FBXBufferViewIndex bvi = dict["bufferView"];
			ERR_FAIL_INDEX_V(bvi, p_state->buffer_views.size(), ERR_PARAMETER_RANGE_ERROR);
			Ref<FBXBufferView> bv = p_state->buffer_views[bvi];
			const FBXBufferIndex bi = bv->buffer;
			ERR_FAIL_INDEX_V(bi, p_state->buffers.size(), ERR_PARAMETER_RANGE_ERROR);
			ERR_FAIL_COND_V(bv->byte_offset + bv->byte_length > p_state->buffers[bi].size(), ERR_FILE_CORRUPT);
			const PackedByteArray &buffer = p_state->buffers[bi];
			data = buffer.slice(bv->byte_offset, bv->byte_offset + bv->byte_length);
		}
		// Done loading the image data bytes. Check that we actually got data to parse.
		// Note: There are paths above that return early, so this point might not be reached.
		if (data.is_empty()) {
			WARN_PRINT(vformat("glTF: Image index '%d' couldn't be loaded, no data found. Skipping it.", i));
			p_state->images.push_back(Ref<Texture2D>()); // Placeholder to keep count.
			p_state->source_images.push_back(Ref<Image>());
			continue;
		}
		// Parse the image data from bytes into an Image resource and save if needed.
		String file_extension;
		Ref<Image> img = _parse_image_bytes_into_image(p_state, data, mime_type, i, file_extension);
		img->set_name(image_name);
		_parse_image_save_image(p_state, data, file_extension, i, img);
	}

	print_verbose("glTF: Total images: " + itos(p_state->images.size()));

	return OK;
}

Error FBXDocument::_parse_textures(Ref<FBXState> p_state) {
	if (!p_state->json.has("textures")) {
		return OK;
	}

	const Array &textures = p_state->json["textures"];
	for (FBXTextureIndex i = 0; i < textures.size(); i++) {
		const Dictionary &texture_dict = textures[i];
		Ref<FBXTexture> gltf_texture;
		gltf_texture.instantiate();
		// Check if any FBXDocumentExtensions want to handle this texture JSON.
		for (Ref<FBXDocumentExtension> ext : document_extensions) {
			ERR_CONTINUE(ext.is_null());
			Error err = ext->parse_texture_json(p_state, texture_dict, gltf_texture);
			ERR_CONTINUE_MSG(err != OK, "GLTF: Encountered error " + itos(err) + " when parsing texture JSON " + String(Variant(texture_dict)) + " in file " + p_state->filename + ". Continuing.");
			if (gltf_texture->get_src_image() != -1) {
				break;
			}
		}
		if (gltf_texture->get_src_image() == -1) {
			// No extensions handled it, so use the base GLTF source.
			// This may be the fallback, or the only option anyway.
			ERR_FAIL_COND_V(!texture_dict.has("source"), ERR_PARSE_ERROR);
			gltf_texture->set_src_image(texture_dict["source"]);
		}
		if (gltf_texture->get_sampler() == -1 && texture_dict.has("sampler")) {
			gltf_texture->set_sampler(texture_dict["sampler"]);
		}
		p_state->textures.push_back(gltf_texture);
	}

	return OK;
}

FBXTextureIndex FBXDocument::_set_texture(Ref<FBXState> p_state, Ref<Texture2D> p_texture, StandardMaterial3D::TextureFilter p_filter_mode, bool p_repeats) {
	ERR_FAIL_COND_V(p_texture.is_null(), -1);
	Ref<FBXTexture> gltf_texture;
	gltf_texture.instantiate();
	ERR_FAIL_COND_V(p_texture->get_image().is_null(), -1);
	FBXImageIndex gltf_src_image_i = p_state->images.size();
	p_state->images.push_back(p_texture);
	p_state->source_images.push_back(p_texture->get_image());
	gltf_texture->set_src_image(gltf_src_image_i);
	gltf_texture->set_sampler(_set_sampler_for_mode(p_state, p_filter_mode, p_repeats));
	FBXTextureIndex gltf_texture_i = p_state->textures.size();
	p_state->textures.push_back(gltf_texture);
	return gltf_texture_i;
}

Ref<Texture2D> FBXDocument::_get_texture(Ref<FBXState> p_state, const FBXTextureIndex p_texture, int p_texture_types) {
	ERR_FAIL_INDEX_V(p_texture, p_state->textures.size(), Ref<Texture2D>());
	const FBXImageIndex image = p_state->textures[p_texture]->get_src_image();
	ERR_FAIL_INDEX_V(image, p_state->images.size(), Ref<Texture2D>());
	if (FBXState::GLTFHandleBinary(p_state->handle_binary_image) == FBXState::GLTFHandleBinary::HANDLE_BINARY_EMBED_AS_BASISU) {
		ERR_FAIL_INDEX_V(image, p_state->source_images.size(), Ref<Texture2D>());
		Ref<PortableCompressedTexture2D> portable_texture;
		portable_texture.instantiate();
		portable_texture->set_keep_compressed_buffer(true);
		Ref<Image> new_img = p_state->source_images[image]->duplicate();
		ERR_FAIL_COND_V(new_img.is_null(), Ref<Texture2D>());
		new_img->generate_mipmaps();
		if (p_texture_types) {
			portable_texture->create_from_image(new_img, PortableCompressedTexture2D::COMPRESSION_MODE_BASIS_UNIVERSAL, true);
		} else {
			portable_texture->create_from_image(new_img, PortableCompressedTexture2D::COMPRESSION_MODE_BASIS_UNIVERSAL, false);
		}
		p_state->images.write[image] = portable_texture;
		p_state->source_images.write[image] = new_img;
	}
	return p_state->images[image];
}

FBXTextureSamplerIndex FBXDocument::_set_sampler_for_mode(Ref<FBXState> p_state, StandardMaterial3D::TextureFilter p_filter_mode, bool p_repeats) {
	for (int i = 0; i < p_state->texture_samplers.size(); ++i) {
		if (p_state->texture_samplers[i]->get_filter_mode() == p_filter_mode) {
			return i;
		}
	}

	FBXTextureSamplerIndex gltf_sampler_i = p_state->texture_samplers.size();
	Ref<FBXTextureSampler> gltf_sampler;
	gltf_sampler.instantiate();
	gltf_sampler->set_filter_mode(p_filter_mode);
	gltf_sampler->set_wrap_mode(p_repeats);
	p_state->texture_samplers.push_back(gltf_sampler);
	return gltf_sampler_i;
}

Ref<FBXTextureSampler> FBXDocument::_get_sampler_for_texture(Ref<FBXState> p_state, const FBXTextureIndex p_texture) {
	ERR_FAIL_INDEX_V(p_texture, p_state->textures.size(), Ref<Texture2D>());
	const FBXTextureSamplerIndex sampler = p_state->textures[p_texture]->get_sampler();

	if (sampler == -1) {
		return p_state->default_texture_sampler;
	} else {
		ERR_FAIL_INDEX_V(sampler, p_state->texture_samplers.size(), Ref<FBXTextureSampler>());

		return p_state->texture_samplers[sampler];
	}
}

Error FBXDocument::_parse_texture_samplers(Ref<FBXState> p_state) {
	p_state->default_texture_sampler.instantiate();
	p_state->default_texture_sampler->set_min_filter(FBXTextureSampler::FilterMode::LINEAR_MIPMAP_LINEAR);
	p_state->default_texture_sampler->set_mag_filter(FBXTextureSampler::FilterMode::LINEAR);
	p_state->default_texture_sampler->set_wrap_s(FBXTextureSampler::WrapMode::REPEAT);
	p_state->default_texture_sampler->set_wrap_t(FBXTextureSampler::WrapMode::REPEAT);

	if (!p_state->json.has("samplers")) {
		return OK;
	}

	const Array &samplers = p_state->json["samplers"];
	for (int i = 0; i < samplers.size(); ++i) {
		const Dictionary &d = samplers[i];

		Ref<FBXTextureSampler> sampler;
		sampler.instantiate();

		if (d.has("minFilter")) {
			sampler->set_min_filter(d["minFilter"]);
		} else {
			sampler->set_min_filter(FBXTextureSampler::FilterMode::LINEAR_MIPMAP_LINEAR);
		}
		if (d.has("magFilter")) {
			sampler->set_mag_filter(d["magFilter"]);
		} else {
			sampler->set_mag_filter(FBXTextureSampler::FilterMode::LINEAR);
		}

		if (d.has("wrapS")) {
			sampler->set_wrap_s(d["wrapS"]);
		} else {
			sampler->set_wrap_s(FBXTextureSampler::WrapMode::DEFAULT);
		}

		if (d.has("wrapT")) {
			sampler->set_wrap_t(d["wrapT"]);
		} else {
			sampler->set_wrap_t(FBXTextureSampler::WrapMode::DEFAULT);
		}

		p_state->texture_samplers.push_back(sampler);
	}

	return OK;
}

Error FBXDocument::_parse_materials(Ref<FBXState> p_state) {
	if (!p_state->json.has("materials")) {
		return OK;
	}

	const Array &materials = p_state->json["materials"];
	for (FBXMaterialIndex i = 0; i < materials.size(); i++) {
		const Dictionary &material_dict = materials[i];

		Ref<StandardMaterial3D> material;
		material.instantiate();
		if (material_dict.has("name") && !String(material_dict["name"]).is_empty()) {
			material->set_name(material_dict["name"]);
		} else {
			material->set_name(vformat("material_%s", itos(i)));
		}
		material->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
		Dictionary material_extensions;
		if (material_dict.has("extensions")) {
			material_extensions = material_dict["extensions"];
		}

		if (material_extensions.has("KHR_materials_unlit")) {
			material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
		}

		if (material_extensions.has("KHR_materials_emissive_strength")) {
			Dictionary emissive_strength = material_extensions["KHR_materials_emissive_strength"];
			if (emissive_strength.has("emissiveStrength")) {
				material->set_emission_energy_multiplier(emissive_strength["emissiveStrength"]);
			}
		}
		if (material_dict.has("pbrMetallicRoughness")) {
			const Dictionary &mr = material_dict["pbrMetallicRoughness"];
			if (mr.has("baseColorFactor")) {
				const Array &arr = mr["baseColorFactor"];
				ERR_FAIL_COND_V(arr.size() != 4, ERR_PARSE_ERROR);
				const Color c = Color(arr[0], arr[1], arr[2], arr[3]).linear_to_srgb();
				material->set_albedo(c);
			}

			if (mr.has("baseColorTexture")) {
				const Dictionary &bct = mr["baseColorTexture"];
				if (bct.has("index")) {
					Ref<FBXTextureSampler> bct_sampler = _get_sampler_for_texture(p_state, bct["index"]);
					material->set_texture_filter(bct_sampler->get_filter_mode());
					material->set_flag(BaseMaterial3D::FLAG_USE_TEXTURE_REPEAT, bct_sampler->get_wrap_mode());
					material->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, _get_texture(p_state, bct["index"], TEXTURE_TYPE_GENERIC));
				}
				if (!mr.has("baseColorFactor")) {
					material->set_albedo(Color(1, 1, 1));
				}
				_set_texture_transform_uv1(bct, material);
			}

			if (mr.has("metallicFactor")) {
				material->set_metallic(mr["metallicFactor"]);
			} else {
				material->set_metallic(1.0);
			}

			if (mr.has("roughnessFactor")) {
				material->set_roughness(mr["roughnessFactor"]);
			} else {
				material->set_roughness(1.0);
			}

			if (mr.has("metallicRoughnessTexture")) {
				const Dictionary &bct = mr["metallicRoughnessTexture"];
				if (bct.has("index")) {
					const Ref<Texture2D> t = _get_texture(p_state, bct["index"], TEXTURE_TYPE_GENERIC);
					material->set_texture(BaseMaterial3D::TEXTURE_METALLIC, t);
					material->set_metallic_texture_channel(BaseMaterial3D::TEXTURE_CHANNEL_BLUE);
					material->set_texture(BaseMaterial3D::TEXTURE_ROUGHNESS, t);
					material->set_roughness_texture_channel(BaseMaterial3D::TEXTURE_CHANNEL_GREEN);
					if (!mr.has("metallicFactor")) {
						material->set_metallic(1);
					}
					if (!mr.has("roughnessFactor")) {
						material->set_roughness(1);
					}
				}
			}
		}

		if (material_dict.has("normalTexture")) {
			const Dictionary &bct = material_dict["normalTexture"];
			if (bct.has("index")) {
				material->set_texture(BaseMaterial3D::TEXTURE_NORMAL, _get_texture(p_state, bct["index"], TEXTURE_TYPE_NORMAL));
				material->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, true);
			}
			if (bct.has("scale")) {
				material->set_normal_scale(bct["scale"]);
			}
		}
		if (material_dict.has("occlusionTexture")) {
			const Dictionary &bct = material_dict["occlusionTexture"];
			if (bct.has("index")) {
				material->set_texture(BaseMaterial3D::TEXTURE_AMBIENT_OCCLUSION, _get_texture(p_state, bct["index"], TEXTURE_TYPE_GENERIC));
				material->set_ao_texture_channel(BaseMaterial3D::TEXTURE_CHANNEL_RED);
				material->set_feature(BaseMaterial3D::FEATURE_AMBIENT_OCCLUSION, true);
			}
		}

		if (material_dict.has("emissiveFactor")) {
			const Array &arr = material_dict["emissiveFactor"];
			ERR_FAIL_COND_V(arr.size() != 3, ERR_PARSE_ERROR);
			const Color c = Color(arr[0], arr[1], arr[2]).linear_to_srgb();
			material->set_feature(BaseMaterial3D::FEATURE_EMISSION, true);

			material->set_emission(c);
		}

		if (material_dict.has("emissiveTexture")) {
			const Dictionary &bct = material_dict["emissiveTexture"];
			if (bct.has("index")) {
				material->set_texture(BaseMaterial3D::TEXTURE_EMISSION, _get_texture(p_state, bct["index"], TEXTURE_TYPE_GENERIC));
				material->set_feature(BaseMaterial3D::FEATURE_EMISSION, true);
				material->set_emission(Color(0, 0, 0));
			}
		}

		if (material_dict.has("doubleSided")) {
			const bool ds = material_dict["doubleSided"];
			if (ds) {
				material->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
			}
		}
		if (material_dict.has("alphaMode")) {
			const String &am = material_dict["alphaMode"];
			if (am == "BLEND") {
				material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_DEPTH_PRE_PASS);
			} else if (am == "MASK") {
				material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR);
				if (material_dict.has("alphaCutoff")) {
					material->set_alpha_scissor_threshold(material_dict["alphaCutoff"]);
				} else {
					material->set_alpha_scissor_threshold(0.5f);
				}
			}
		}
		p_state->materials.push_back(material);
	}

	print_verbose("Total materials: " + itos(p_state->materials.size()));

	return OK;
}

void FBXDocument::_set_texture_transform_uv1(const Dictionary &p_dict, Ref<BaseMaterial3D> p_material) {
	if (p_dict.has("extensions")) {
		const Dictionary &extensions = p_dict["extensions"];
		if (extensions.has("KHR_texture_transform")) {
			if (p_material.is_valid()) {
				const Dictionary &texture_transform = extensions["KHR_texture_transform"];
				const Array &offset_arr = texture_transform["offset"];
				if (offset_arr.size() == 2) {
					const Vector3 offset_vector3 = Vector3(offset_arr[0], offset_arr[1], 0.0f);
					p_material->set_uv1_offset(offset_vector3);
				}

				const Array &scale_arr = texture_transform["scale"];
				if (scale_arr.size() == 2) {
					const Vector3 scale_vector3 = Vector3(scale_arr[0], scale_arr[1], 1.0f);
					p_material->set_uv1_scale(scale_vector3);
				}
			}
		}
	}
}

void FBXDocument::spec_gloss_to_metal_base_color(const Color &p_specular_factor, const Color &p_diffuse, Color &r_base_color, float &r_metallic) {
	const Color DIELECTRIC_SPECULAR = Color(0.04f, 0.04f, 0.04f);
	Color specular = Color(p_specular_factor.r, p_specular_factor.g, p_specular_factor.b);
	const float one_minus_specular_strength = 1.0f - get_max_component(specular);
	const float dielectric_specular_red = DIELECTRIC_SPECULAR.r;
	float brightness_diffuse = get_perceived_brightness(p_diffuse);
	const float brightness_specular = get_perceived_brightness(specular);
	r_metallic = solve_metallic(dielectric_specular_red, brightness_diffuse, brightness_specular, one_minus_specular_strength);
	const float one_minus_metallic = 1.0f - r_metallic;
	const Color base_color_from_diffuse = p_diffuse * (one_minus_specular_strength / (1.0f - dielectric_specular_red) / MAX(one_minus_metallic, CMP_EPSILON));
	const Color base_color_from_specular = (specular - (DIELECTRIC_SPECULAR * (one_minus_metallic))) * (1.0f / MAX(r_metallic, CMP_EPSILON));
	r_base_color.r = Math::lerp(base_color_from_diffuse.r, base_color_from_specular.r, r_metallic * r_metallic);
	r_base_color.g = Math::lerp(base_color_from_diffuse.g, base_color_from_specular.g, r_metallic * r_metallic);
	r_base_color.b = Math::lerp(base_color_from_diffuse.b, base_color_from_specular.b, r_metallic * r_metallic);
	r_base_color.a = p_diffuse.a;
	r_base_color = r_base_color.clamp();
}

FBXNodeIndex FBXDocument::_find_highest_node(Ref<FBXState> p_state, const Vector<FBXNodeIndex> &p_subset) {
	int highest = -1;
	FBXNodeIndex best_node = -1;

	for (int i = 0; i < p_subset.size(); ++i) {
		const FBXNodeIndex node_i = p_subset[i];
		const Ref<FBXNode> node = p_state->nodes[node_i];

		if (highest == -1 || node->height < highest) {
			highest = node->height;
			best_node = node_i;
		}
	}

	return best_node;
}

bool FBXDocument::_capture_nodes_in_skin(Ref<FBXState> p_state, Ref<FBXSkin> p_skin, const FBXNodeIndex p_node_index) {
	bool found_joint = false;

	for (int i = 0; i < p_state->nodes[p_node_index]->children.size(); ++i) {
		found_joint |= _capture_nodes_in_skin(p_state, p_skin, p_state->nodes[p_node_index]->children[i]);
	}

	if (found_joint) {
		// Mark it if we happen to find another skins joint...
		if (p_state->nodes[p_node_index]->joint && p_skin->joints.find(p_node_index) < 0) {
			p_skin->joints.push_back(p_node_index);
		} else if (p_skin->non_joints.find(p_node_index) < 0) {
			p_skin->non_joints.push_back(p_node_index);
		}
	}

	if (p_skin->joints.find(p_node_index) > 0) {
		return true;
	}

	return false;
}

void FBXDocument::_capture_nodes_for_multirooted_skin(Ref<FBXState> p_state, Ref<FBXSkin> p_skin) {
	DisjointSet<FBXNodeIndex> disjoint_set;

	for (int i = 0; i < p_skin->joints.size(); ++i) {
		const FBXNodeIndex node_index = p_skin->joints[i];
		const FBXNodeIndex parent = p_state->nodes[node_index]->parent;
		disjoint_set.insert(node_index);

		if (p_skin->joints.find(parent) >= 0) {
			disjoint_set.create_union(parent, node_index);
		}
	}

	Vector<FBXNodeIndex> roots;
	disjoint_set.get_representatives(roots);

	if (roots.size() <= 1) {
		return;
	}

	int maxHeight = -1;

	// Determine the max height rooted tree
	for (int i = 0; i < roots.size(); ++i) {
		const FBXNodeIndex root = roots[i];

		if (maxHeight == -1 || p_state->nodes[root]->height < maxHeight) {
			maxHeight = p_state->nodes[root]->height;
		}
	}

	// Go up the tree till all of the multiple roots of the skin are at the same hierarchy level.
	// This sucks, but 99% of all game engines (not just Godot) would have this same issue.
	for (int i = 0; i < roots.size(); ++i) {
		FBXNodeIndex current_node = roots[i];
		while (p_state->nodes[current_node]->height > maxHeight) {
			FBXNodeIndex parent = p_state->nodes[current_node]->parent;

			if (p_state->nodes[parent]->joint && p_skin->joints.find(parent) < 0) {
				p_skin->joints.push_back(parent);
			} else if (p_skin->non_joints.find(parent) < 0) {
				p_skin->non_joints.push_back(parent);
			}

			current_node = parent;
		}

		// replace the roots
		roots.write[i] = current_node;
	}

	// Climb up the tree until they all have the same parent
	bool all_same;

	do {
		all_same = true;
		const FBXNodeIndex first_parent = p_state->nodes[roots[0]]->parent;

		for (int i = 1; i < roots.size(); ++i) {
			all_same &= (first_parent == p_state->nodes[roots[i]]->parent);
		}

		if (!all_same) {
			for (int i = 0; i < roots.size(); ++i) {
				const FBXNodeIndex current_node = roots[i];
				const FBXNodeIndex parent = p_state->nodes[current_node]->parent;

				if (p_state->nodes[parent]->joint && p_skin->joints.find(parent) < 0) {
					p_skin->joints.push_back(parent);
				} else if (p_skin->non_joints.find(parent) < 0) {
					p_skin->non_joints.push_back(parent);
				}

				roots.write[i] = parent;
			}
		}

	} while (!all_same);
}

Error FBXDocument::_expand_skin(Ref<FBXState> p_state, Ref<FBXSkin> p_skin) {
	_capture_nodes_for_multirooted_skin(p_state, p_skin);

	// Grab all nodes that lay in between skin joints/nodes
	DisjointSet<FBXNodeIndex> disjoint_set;

	Vector<FBXNodeIndex> all_skin_nodes;
	all_skin_nodes.append_array(p_skin->joints);
	all_skin_nodes.append_array(p_skin->non_joints);

	for (int i = 0; i < all_skin_nodes.size(); ++i) {
		const FBXNodeIndex node_index = all_skin_nodes[i];
		const FBXNodeIndex parent = p_state->nodes[node_index]->parent;
		disjoint_set.insert(node_index);

		if (all_skin_nodes.find(parent) >= 0) {
			disjoint_set.create_union(parent, node_index);
		}
	}

	Vector<FBXNodeIndex> out_owners;
	disjoint_set.get_representatives(out_owners);

	Vector<FBXNodeIndex> out_roots;

	for (int i = 0; i < out_owners.size(); ++i) {
		Vector<FBXNodeIndex> set;
		disjoint_set.get_members(set, out_owners[i]);

		const FBXNodeIndex root = _find_highest_node(p_state, set);
		ERR_FAIL_COND_V(root < 0, FAILED);
		out_roots.push_back(root);
	}

	out_roots.sort();

	for (int i = 0; i < out_roots.size(); ++i) {
		_capture_nodes_in_skin(p_state, p_skin, out_roots[i]);
	}

	p_skin->roots = out_roots;

	return OK;
}

Error FBXDocument::_verify_skin(Ref<FBXState> p_state, Ref<FBXSkin> p_skin) {
	// This may seem duplicated from expand_skins, but this is really a sanity check! (so it kinda is)
	// In case additional interpolating logic is added to the skins, this will help ensure that you
	// do not cause it to self implode into a fiery blaze

	// We are going to re-calculate the root nodes and compare them to the ones saved in the skin,
	// then ensure the multiple trees (if they exist) are on the same sublevel

	// Grab all nodes that lay in between skin joints/nodes
	DisjointSet<FBXNodeIndex> disjoint_set;

	Vector<FBXNodeIndex> all_skin_nodes;
	all_skin_nodes.append_array(p_skin->joints);
	all_skin_nodes.append_array(p_skin->non_joints);

	for (int i = 0; i < all_skin_nodes.size(); ++i) {
		const FBXNodeIndex node_index = all_skin_nodes[i];
		const FBXNodeIndex parent = p_state->nodes[node_index]->parent;
		disjoint_set.insert(node_index);

		if (all_skin_nodes.find(parent) >= 0) {
			disjoint_set.create_union(parent, node_index);
		}
	}

	Vector<FBXNodeIndex> out_owners;
	disjoint_set.get_representatives(out_owners);

	Vector<FBXNodeIndex> out_roots;

	for (int i = 0; i < out_owners.size(); ++i) {
		Vector<FBXNodeIndex> set;
		disjoint_set.get_members(set, out_owners[i]);

		const FBXNodeIndex root = _find_highest_node(p_state, set);
		ERR_FAIL_COND_V(root < 0, FAILED);
		out_roots.push_back(root);
	}

	out_roots.sort();

	ERR_FAIL_COND_V(out_roots.size() == 0, FAILED);

	// Make sure the roots are the exact same (they better be)
	ERR_FAIL_COND_V(out_roots.size() != p_skin->roots.size(), FAILED);
	for (int i = 0; i < out_roots.size(); ++i) {
		ERR_FAIL_COND_V(out_roots[i] != p_skin->roots[i], FAILED);
	}

	// Single rooted skin? Perfectly ok!
	if (out_roots.size() == 1) {
		return OK;
	}

	// Make sure all parents of a multi-rooted skin are the SAME
	const FBXNodeIndex parent = p_state->nodes[out_roots[0]]->parent;
	for (int i = 1; i < out_roots.size(); ++i) {
		if (p_state->nodes[out_roots[i]]->parent != parent) {
			return FAILED;
		}
	}

	return OK;
}

Error FBXDocument::_parse_skins(Ref<FBXState> p_state) {
	if (!p_state->json.has("skins")) {
		return OK;
	}

	const Array &skins = p_state->json["skins"];

	// Create the base skins, and mark nodes that are joints
	for (int i = 0; i < skins.size(); i++) {
		const Dictionary &d = skins[i];

		Ref<FBXSkin> skin;
		skin.instantiate();

		ERR_FAIL_COND_V(!d.has("joints"), ERR_PARSE_ERROR);

		const Array &joints = d["joints"];

		if (d.has("inverseBindMatrices")) {
			skin->inverse_binds = _decode_accessor_as_xform(p_state, d["inverseBindMatrices"], false);
			ERR_FAIL_COND_V(skin->inverse_binds.size() != joints.size(), ERR_PARSE_ERROR);
		}

		for (int j = 0; j < joints.size(); j++) {
			const FBXNodeIndex node = joints[j];
			ERR_FAIL_INDEX_V(node, p_state->nodes.size(), ERR_PARSE_ERROR);

			skin->joints.push_back(node);
			skin->joints_original.push_back(node);

			p_state->nodes.write[node]->joint = true;
		}

		if (d.has("name") && !String(d["name"]).is_empty()) {
			skin->set_name(d["name"]);
		} else {
			skin->set_name(vformat("skin_%s", itos(i)));
		}

		if (d.has("skeleton")) {
			skin->skin_root = d["skeleton"];
		}

		p_state->skins.push_back(skin);
	}

	for (FBXSkinIndex i = 0; i < p_state->skins.size(); ++i) {
		Ref<FBXSkin> skin = p_state->skins.write[i];

		// Expand the skin to capture all the extra non-joints that lie in between the actual joints,
		// and expand the hierarchy to ensure multi-rooted trees lie on the same height level
		ERR_FAIL_COND_V(_expand_skin(p_state, skin), ERR_PARSE_ERROR);
		ERR_FAIL_COND_V(_verify_skin(p_state, skin), ERR_PARSE_ERROR);
	}

	print_verbose("glTF: Total skins: " + itos(p_state->skins.size()));

	return OK;
}

void FBXDocument::_recurse_children(Ref<FBXState> p_state, const FBXNodeIndex p_node_index,
		RBSet<FBXNodeIndex> &p_all_skin_nodes, HashSet<FBXNodeIndex> &p_child_visited_set) {
	if (p_child_visited_set.has(p_node_index)) {
		return;
	}
	p_child_visited_set.insert(p_node_index);
	for (int i = 0; i < p_state->nodes[p_node_index]->children.size(); ++i) {
		_recurse_children(p_state, p_state->nodes[p_node_index]->children[i], p_all_skin_nodes, p_child_visited_set);
	}

	if (p_state->nodes[p_node_index]->skin < 0 || p_state->nodes[p_node_index]->mesh < 0 || !p_state->nodes[p_node_index]->children.is_empty()) {
		p_all_skin_nodes.insert(p_node_index);
	}
}

Error FBXDocument::_determine_skeletons(Ref<FBXState> p_state) {
	// Using a disjoint set, we are going to potentially combine all skins that are actually branches
	// of a main skeleton, or treat skins defining the same set of nodes as ONE skeleton.
	// This is another unclear issue caused by the current glTF specification.

	DisjointSet<FBXNodeIndex> skeleton_sets;

	for (FBXSkinIndex skin_i = 0; skin_i < p_state->skins.size(); ++skin_i) {
		const Ref<FBXSkin> skin = p_state->skins[skin_i];

		HashSet<FBXNodeIndex> child_visited_set;
		RBSet<FBXNodeIndex> all_skin_nodes;
		for (int i = 0; i < skin->joints.size(); ++i) {
			all_skin_nodes.insert(skin->joints[i]);
			_recurse_children(p_state, skin->joints[i], all_skin_nodes, child_visited_set);
		}
		for (int i = 0; i < skin->non_joints.size(); ++i) {
			all_skin_nodes.insert(skin->non_joints[i]);
			_recurse_children(p_state, skin->non_joints[i], all_skin_nodes, child_visited_set);
		}
		for (FBXNodeIndex node_index : all_skin_nodes) {
			const FBXNodeIndex parent = p_state->nodes[node_index]->parent;
			skeleton_sets.insert(node_index);

			if (all_skin_nodes.has(parent)) {
				skeleton_sets.create_union(parent, node_index);
			}
		}

		// We are going to connect the separate skin subtrees in each skin together
		// so that the final roots are entire sets of valid skin trees
		for (int i = 1; i < skin->roots.size(); ++i) {
			skeleton_sets.create_union(skin->roots[0], skin->roots[i]);
		}
	}

	{ // attempt to joint all touching subsets (siblings/parent are part of another skin)
		Vector<FBXNodeIndex> groups_representatives;
		skeleton_sets.get_representatives(groups_representatives);

		Vector<FBXNodeIndex> highest_group_members;
		Vector<Vector<FBXNodeIndex>> groups;
		for (int i = 0; i < groups_representatives.size(); ++i) {
			Vector<FBXNodeIndex> group;
			skeleton_sets.get_members(group, groups_representatives[i]);
			highest_group_members.push_back(_find_highest_node(p_state, group));
			groups.push_back(group);
		}

		for (int i = 0; i < highest_group_members.size(); ++i) {
			const FBXNodeIndex node_i = highest_group_members[i];

			// Attach any siblings together (this needs to be done n^2/2 times)
			for (int j = i + 1; j < highest_group_members.size(); ++j) {
				const FBXNodeIndex node_j = highest_group_members[j];

				// Even if they are siblings under the root! :)
				if (p_state->nodes[node_i]->parent == p_state->nodes[node_j]->parent) {
					skeleton_sets.create_union(node_i, node_j);
				}
			}

			// Attach any parenting going on together (we need to do this n^2 times)
			const FBXNodeIndex node_i_parent = p_state->nodes[node_i]->parent;
			if (node_i_parent >= 0) {
				for (int j = 0; j < groups.size() && i != j; ++j) {
					const Vector<FBXNodeIndex> &group = groups[j];

					if (group.find(node_i_parent) >= 0) {
						const FBXNodeIndex node_j = highest_group_members[j];
						skeleton_sets.create_union(node_i, node_j);
					}
				}
			}
		}
	}

	// At this point, the skeleton groups should be finalized
	Vector<FBXNodeIndex> skeleton_owners;
	skeleton_sets.get_representatives(skeleton_owners);

	// Mark all the skins actual skeletons, after we have merged them
	for (FBXSkeletonIndex skel_i = 0; skel_i < skeleton_owners.size(); ++skel_i) {
		const FBXNodeIndex skeleton_owner = skeleton_owners[skel_i];
		Ref<FBXSkeleton> skeleton;
		skeleton.instantiate();

		Vector<FBXNodeIndex> skeleton_nodes;
		skeleton_sets.get_members(skeleton_nodes, skeleton_owner);

		for (FBXSkinIndex skin_i = 0; skin_i < p_state->skins.size(); ++skin_i) {
			Ref<FBXSkin> skin = p_state->skins.write[skin_i];

			// If any of the the skeletons nodes exist in a skin, that skin now maps to the skeleton
			for (int i = 0; i < skeleton_nodes.size(); ++i) {
				FBXNodeIndex skel_node_i = skeleton_nodes[i];
				if (skin->joints.find(skel_node_i) >= 0 || skin->non_joints.find(skel_node_i) >= 0) {
					skin->skeleton = skel_i;
					continue;
				}
			}
		}

		Vector<FBXNodeIndex> non_joints;
		for (int i = 0; i < skeleton_nodes.size(); ++i) {
			const FBXNodeIndex node_i = skeleton_nodes[i];

			if (p_state->nodes[node_i]->joint) {
				skeleton->joints.push_back(node_i);
			} else {
				non_joints.push_back(node_i);
			}
		}

		p_state->skeletons.push_back(skeleton);

		_reparent_non_joint_skeleton_subtrees(p_state, p_state->skeletons.write[skel_i], non_joints);
	}

	for (FBXSkeletonIndex skel_i = 0; skel_i < p_state->skeletons.size(); ++skel_i) {
		Ref<FBXSkeleton> skeleton = p_state->skeletons.write[skel_i];

		for (int i = 0; i < skeleton->joints.size(); ++i) {
			const FBXNodeIndex node_i = skeleton->joints[i];
			Ref<FBXNode> node = p_state->nodes[node_i];

			ERR_FAIL_COND_V(!node->joint, ERR_PARSE_ERROR);
			ERR_FAIL_COND_V(node->skeleton >= 0, ERR_PARSE_ERROR);
			node->skeleton = skel_i;
		}

		ERR_FAIL_COND_V(_determine_skeleton_roots(p_state, skel_i), ERR_PARSE_ERROR);
	}

	return OK;
}

Error FBXDocument::_reparent_non_joint_skeleton_subtrees(Ref<FBXState> p_state, Ref<FBXSkeleton> p_skeleton, const Vector<FBXNodeIndex> &p_non_joints) {
	DisjointSet<FBXNodeIndex> subtree_set;

	// Populate the disjoint set with ONLY non joints that are in the skeleton hierarchy (non_joints vector)
	// This way we can find any joints that lie in between joints, as the current glTF specification
	// mentions nothing about non-joints being in between joints of the same skin. Hopefully one day we
	// can remove this code.

	// skinD depicted here explains this issue:
	// https://github.com/KhronosGroup/glTF-Asset-Generator/blob/master/Output/Positive/Animation_Skin

	for (int i = 0; i < p_non_joints.size(); ++i) {
		const FBXNodeIndex node_i = p_non_joints[i];

		subtree_set.insert(node_i);

		const FBXNodeIndex parent_i = p_state->nodes[node_i]->parent;
		if (parent_i >= 0 && p_non_joints.find(parent_i) >= 0 && !p_state->nodes[parent_i]->joint) {
			subtree_set.create_union(parent_i, node_i);
		}
	}

	// Find all the non joint subtrees and re-parent them to a new "fake" joint

	Vector<FBXNodeIndex> non_joint_subtree_roots;
	subtree_set.get_representatives(non_joint_subtree_roots);

	for (int root_i = 0; root_i < non_joint_subtree_roots.size(); ++root_i) {
		const FBXNodeIndex subtree_root = non_joint_subtree_roots[root_i];

		Vector<FBXNodeIndex> subtree_nodes;
		subtree_set.get_members(subtree_nodes, subtree_root);

		for (int subtree_i = 0; subtree_i < subtree_nodes.size(); ++subtree_i) {
			Ref<FBXNode> node = p_state->nodes[subtree_nodes[subtree_i]];
			node->joint = true;
			// Add the joint to the skeletons joints
			p_skeleton->joints.push_back(subtree_nodes[subtree_i]);
		}
	}

	return OK;
}

Error FBXDocument::_determine_skeleton_roots(Ref<FBXState> p_state, const FBXSkeletonIndex p_skel_i) {
	DisjointSet<FBXNodeIndex> disjoint_set;

	for (FBXNodeIndex i = 0; i < p_state->nodes.size(); ++i) {
		const Ref<FBXNode> node = p_state->nodes[i];

		if (node->skeleton != p_skel_i) {
			continue;
		}

		disjoint_set.insert(i);

		if (node->parent >= 0 && p_state->nodes[node->parent]->skeleton == p_skel_i) {
			disjoint_set.create_union(node->parent, i);
		}
	}

	Ref<FBXSkeleton> skeleton = p_state->skeletons.write[p_skel_i];

	Vector<FBXNodeIndex> representatives;
	disjoint_set.get_representatives(representatives);

	Vector<FBXNodeIndex> roots;

	for (int i = 0; i < representatives.size(); ++i) {
		Vector<FBXNodeIndex> set;
		disjoint_set.get_members(set, representatives[i]);
		const FBXNodeIndex root = _find_highest_node(p_state, set);
		ERR_FAIL_COND_V(root < 0, FAILED);
		roots.push_back(root);
	}

	roots.sort();

	skeleton->roots = roots;

	if (roots.size() == 0) {
		return FAILED;
	} else if (roots.size() == 1) {
		return OK;
	}

	// Check that the subtrees have the same parent root
	const FBXNodeIndex parent = p_state->nodes[roots[0]]->parent;
	for (int i = 1; i < roots.size(); ++i) {
		if (p_state->nodes[roots[i]]->parent != parent) {
			return FAILED;
		}
	}

	return OK;
}

Error FBXDocument::_create_skeletons(Ref<FBXState> p_state) {
	for (FBXSkeletonIndex skel_i = 0; skel_i < p_state->skeletons.size(); ++skel_i) {
		Ref<FBXSkeleton> gltf_skeleton = p_state->skeletons.write[skel_i];

		Skeleton3D *skeleton = memnew(Skeleton3D);
		gltf_skeleton->godot_skeleton = skeleton;
		p_state->skeleton3d_to_gltf_skeleton[skeleton->get_instance_id()] = skel_i;

		// Make a unique name, no gltf node represents this skeleton
		skeleton->set_name("Skeleton3D");

		List<FBXNodeIndex> bones;

		for (int i = 0; i < gltf_skeleton->roots.size(); ++i) {
			bones.push_back(gltf_skeleton->roots[i]);
		}

		// Make the skeleton creation deterministic by going through the roots in
		// a sorted order, and DEPTH FIRST
		bones.sort();

		while (!bones.is_empty()) {
			const FBXNodeIndex node_i = bones.front()->get();
			bones.pop_front();

			Ref<FBXNode> node = p_state->nodes[node_i];
			ERR_FAIL_COND_V(node->skeleton != skel_i, FAILED);

			{ // Add all child nodes to the stack (deterministically)
				Vector<FBXNodeIndex> child_nodes;
				for (int i = 0; i < node->children.size(); ++i) {
					const FBXNodeIndex child_i = node->children[i];
					if (p_state->nodes[child_i]->skeleton == skel_i) {
						child_nodes.push_back(child_i);
					}
				}

				// Depth first insertion
				child_nodes.sort();
				for (int i = child_nodes.size() - 1; i >= 0; --i) {
					bones.push_front(child_nodes[i]);
				}
			}

			const int bone_index = skeleton->get_bone_count();

			if (node->get_name().is_empty()) {
				node->set_name("bone");
			}

			node->set_name(_gen_unique_bone_name(p_state, skel_i, node->get_name()));

			skeleton->add_bone(node->get_name());
			skeleton->set_bone_rest(bone_index, node->xform);
			skeleton->set_bone_pose_position(bone_index, node->position);
			skeleton->set_bone_pose_rotation(bone_index, node->rotation.normalized());
			skeleton->set_bone_pose_scale(bone_index, node->scale);

			if (node->parent >= 0 && p_state->nodes[node->parent]->skeleton == skel_i) {
				const int bone_parent = skeleton->find_bone(p_state->nodes[node->parent]->get_name());
				ERR_FAIL_COND_V(bone_parent < 0, FAILED);
				skeleton->set_bone_parent(bone_index, skeleton->find_bone(p_state->nodes[node->parent]->get_name()));
			}

			p_state->scene_nodes.insert(node_i, skeleton);
		}
	}

	ERR_FAIL_COND_V(_map_skin_joints_indices_to_skeleton_bone_indices(p_state), ERR_PARSE_ERROR);

	return OK;
}

Error FBXDocument::_map_skin_joints_indices_to_skeleton_bone_indices(Ref<FBXState> p_state) {
	for (FBXSkinIndex skin_i = 0; skin_i < p_state->skins.size(); ++skin_i) {
		Ref<FBXSkin> skin = p_state->skins.write[skin_i];

		Ref<FBXSkeleton> skeleton = p_state->skeletons[skin->skeleton];

		for (int joint_index = 0; joint_index < skin->joints_original.size(); ++joint_index) {
			const FBXNodeIndex node_i = skin->joints_original[joint_index];
			const Ref<FBXNode> node = p_state->nodes[node_i];

			const int bone_index = skeleton->godot_skeleton->find_bone(node->get_name());
			ERR_FAIL_COND_V(bone_index < 0, FAILED);

			skin->joint_i_to_bone_i.insert(joint_index, bone_index);
		}
	}

	return OK;
}

Error FBXDocument::_create_skins(Ref<FBXState> p_state) {
	for (FBXSkinIndex skin_i = 0; skin_i < p_state->skins.size(); ++skin_i) {
		Ref<FBXSkin> gltf_skin = p_state->skins.write[skin_i];

		Ref<Skin> skin;
		skin.instantiate();

		// Some skins don't have IBM's! What absolute monsters!
		const bool has_ibms = !gltf_skin->inverse_binds.is_empty();

		for (int joint_i = 0; joint_i < gltf_skin->joints_original.size(); ++joint_i) {
			FBXNodeIndex node = gltf_skin->joints_original[joint_i];
			String bone_name = p_state->nodes[node]->get_name();

			Transform3D xform;
			if (has_ibms) {
				xform = gltf_skin->inverse_binds[joint_i];
			}

			if (p_state->use_named_skin_binds) {
				skin->add_named_bind(bone_name, xform);
			} else {
				int32_t bone_i = gltf_skin->joint_i_to_bone_i[joint_i];
				skin->add_bind(bone_i, xform);
			}
		}

		gltf_skin->godot_skin = skin;
	}

	// Purge the duplicates!
	_remove_duplicate_skins(p_state);

	// Create unique names now, after removing duplicates
	for (FBXSkinIndex skin_i = 0; skin_i < p_state->skins.size(); ++skin_i) {
		Ref<Skin> skin = p_state->skins.write[skin_i]->godot_skin;
		if (skin->get_name().is_empty()) {
			// Make a unique name, no gltf node represents this skin
			skin->set_name(_gen_unique_name(p_state, "Skin"));
		}
	}

	return OK;
}

bool FBXDocument::_skins_are_same(const Ref<Skin> p_skin_a, const Ref<Skin> p_skin_b) {
	if (p_skin_a->get_bind_count() != p_skin_b->get_bind_count()) {
		return false;
	}

	for (int i = 0; i < p_skin_a->get_bind_count(); ++i) {
		if (p_skin_a->get_bind_bone(i) != p_skin_b->get_bind_bone(i)) {
			return false;
		}
		if (p_skin_a->get_bind_name(i) != p_skin_b->get_bind_name(i)) {
			return false;
		}

		Transform3D a_xform = p_skin_a->get_bind_pose(i);
		Transform3D b_xform = p_skin_b->get_bind_pose(i);

		if (a_xform != b_xform) {
			return false;
		}
	}

	return true;
}

void FBXDocument::_remove_duplicate_skins(Ref<FBXState> p_state) {
	for (int i = 0; i < p_state->skins.size(); ++i) {
		for (int j = i + 1; j < p_state->skins.size(); ++j) {
			const Ref<Skin> skin_i = p_state->skins[i]->godot_skin;
			const Ref<Skin> skin_j = p_state->skins[j]->godot_skin;

			if (_skins_are_same(skin_i, skin_j)) {
				// replace it and delete the old
				p_state->skins.write[j]->godot_skin = skin_i;
			}
		}
	}
}

Error FBXDocument::_parse_cameras(Ref<FBXState> p_state) {
	if (!p_state->json.has("cameras")) {
		return OK;
	}

	const Array cameras = p_state->json["cameras"];

	for (FBXCameraIndex i = 0; i < cameras.size(); i++) {
		p_state->cameras.push_back(FBXCamera::from_dictionary(cameras[i]));
	}

	print_verbose("glTF: Total cameras: " + itos(p_state->cameras.size()));

	return OK;
}

String FBXDocument::interpolation_to_string(const FBXAnimation::Interpolation p_interp) {
	String interp = "LINEAR";
	if (p_interp == FBXAnimation::INTERP_STEP) {
		interp = "STEP";
	} else if (p_interp == FBXAnimation::INTERP_LINEAR) {
		interp = "LINEAR";
	} else if (p_interp == FBXAnimation::INTERP_CATMULLROMSPLINE) {
		interp = "CATMULLROMSPLINE";
	} else if (p_interp == FBXAnimation::INTERP_CUBIC_SPLINE) {
		interp = "CUBICSPLINE";
	}

	return interp;
}

Error FBXDocument::_parse_animations(Ref<FBXState> p_state) {
	if (!p_state->json.has("animations")) {
		return OK;
	}

	const Array &animations = p_state->json["animations"];

	for (FBXAnimationIndex i = 0; i < animations.size(); i++) {
		const Dictionary &d = animations[i];

		Ref<FBXAnimation> animation;
		animation.instantiate();

		if (!d.has("channels") || !d.has("samplers")) {
			continue;
		}

		Array channels = d["channels"];
		Array samplers = d["samplers"];

		if (d.has("name")) {
			const String anim_name = d["name"];
			const String anim_name_lower = anim_name.to_lower();
			if (anim_name_lower.begins_with("loop") || anim_name_lower.ends_with("loop") || anim_name_lower.begins_with("cycle") || anim_name_lower.ends_with("cycle")) {
				animation->set_loop(true);
			}
			animation->set_name(_gen_unique_animation_name(p_state, anim_name));
		}

		for (int j = 0; j < channels.size(); j++) {
			const Dictionary &c = channels[j];
			if (!c.has("target")) {
				continue;
			}

			const Dictionary &t = c["target"];
			if (!t.has("node") || !t.has("path")) {
				continue;
			}

			ERR_FAIL_COND_V(!c.has("sampler"), ERR_PARSE_ERROR);
			const int sampler = c["sampler"];
			ERR_FAIL_INDEX_V(sampler, samplers.size(), ERR_PARSE_ERROR);

			FBXNodeIndex node = t["node"];
			String path = t["path"];

			ERR_FAIL_INDEX_V(node, p_state->nodes.size(), ERR_PARSE_ERROR);

			FBXAnimation::Track *track = nullptr;

			if (!animation->get_tracks().has(node)) {
				animation->get_tracks()[node] = FBXAnimation::Track();
			}

			track = &animation->get_tracks()[node];

			const Dictionary &s = samplers[sampler];

			ERR_FAIL_COND_V(!s.has("input"), ERR_PARSE_ERROR);
			ERR_FAIL_COND_V(!s.has("output"), ERR_PARSE_ERROR);

			const int input = s["input"];
			const int output = s["output"];

			FBXAnimation::Interpolation interp = FBXAnimation::INTERP_LINEAR;
			int output_count = 1;
			if (s.has("interpolation")) {
				const String &in = s["interpolation"];
				if (in == "STEP") {
					interp = FBXAnimation::INTERP_STEP;
				} else if (in == "LINEAR") {
					interp = FBXAnimation::INTERP_LINEAR;
				} else if (in == "CATMULLROMSPLINE") {
					interp = FBXAnimation::INTERP_CATMULLROMSPLINE;
					output_count = 3;
				} else if (in == "CUBICSPLINE") {
					interp = FBXAnimation::INTERP_CUBIC_SPLINE;
					output_count = 3;
				}
			}

			const Vector<float> times = _decode_accessor_as_floats(p_state, input, false);
			if (path == "translation") {
				const Vector<Vector3> positions = _decode_accessor_as_vec3(p_state, output, false);
				track->position_track.interpolation = interp;
				track->position_track.times = Variant(times); //convert via variant
				track->position_track.values = Variant(positions); //convert via variant
			} else if (path == "rotation") {
				const Vector<Quaternion> rotations = _decode_accessor_as_quaternion(p_state, output, false);
				track->rotation_track.interpolation = interp;
				track->rotation_track.times = Variant(times); //convert via variant
				track->rotation_track.values = rotations;
			} else if (path == "scale") {
				const Vector<Vector3> scales = _decode_accessor_as_vec3(p_state, output, false);
				track->scale_track.interpolation = interp;
				track->scale_track.times = Variant(times); //convert via variant
				track->scale_track.values = Variant(scales); //convert via variant
			} else if (path == "weights") {
				const Vector<float> weights = _decode_accessor_as_floats(p_state, output, false);

				ERR_FAIL_INDEX_V(p_state->nodes[node]->mesh, p_state->meshes.size(), ERR_PARSE_ERROR);
				Ref<FBXMesh> mesh = p_state->meshes[p_state->nodes[node]->mesh];
				ERR_CONTINUE(!mesh->get_blend_weights().size());
				const int wc = mesh->get_blend_weights().size();

				track->weight_tracks.resize(wc);

				const int expected_value_count = times.size() * output_count * wc;
				ERR_CONTINUE_MSG(weights.size() != expected_value_count, "Invalid weight data, expected " + itos(expected_value_count) + " weight values, got " + itos(weights.size()) + " instead.");

				const int wlen = weights.size() / wc;
				for (int k = 0; k < wc; k++) { //separate tracks, having them together is not such a good idea
					FBXAnimation::Channel<real_t> cf;
					cf.interpolation = interp;
					cf.times = Variant(times);
					Vector<real_t> wdata;
					wdata.resize(wlen);
					for (int l = 0; l < wlen; l++) {
						wdata.write[l] = weights[l * wc + k];
					}

					cf.values = wdata;
					track->weight_tracks.write[k] = cf;
				}
			} else {
				WARN_PRINT("Invalid path '" + path + "'.");
			}
		}

		p_state->animations.push_back(animation);
	}

	print_verbose("glTF: Total animations '" + itos(p_state->animations.size()) + "'.");

	return OK;
}

void FBXDocument::_assign_node_names(Ref<FBXState> p_state) {
	for (int i = 0; i < p_state->nodes.size(); i++) {
		Ref<FBXNode> gltf_node = p_state->nodes[i];

		// Any joints get unique names generated when the skeleton is made, unique to the skeleton
		if (gltf_node->skeleton >= 0) {
			continue;
		}

		if (gltf_node->get_name().is_empty()) {
			if (gltf_node->mesh >= 0) {
				gltf_node->set_name(_gen_unique_name(p_state, "Mesh"));
			} else if (gltf_node->camera >= 0) {
				gltf_node->set_name(_gen_unique_name(p_state, "Camera3D"));
			} else {
				gltf_node->set_name(_gen_unique_name(p_state, "Node"));
			}
		}

		gltf_node->set_name(_gen_unique_name(p_state, gltf_node->get_name()));
	}
}

BoneAttachment3D *FBXDocument::_generate_bone_attachment(Ref<FBXState> p_state, Skeleton3D *p_skeleton, const FBXNodeIndex p_node_index, const FBXNodeIndex p_bone_index) {
	Ref<FBXNode> gltf_node = p_state->nodes[p_node_index];
	Ref<FBXNode> bone_node = p_state->nodes[p_bone_index];
	BoneAttachment3D *bone_attachment = memnew(BoneAttachment3D);
	print_verbose("glTF: Creating bone attachment for: " + gltf_node->get_name());

	ERR_FAIL_COND_V(!bone_node->joint, nullptr);

	bone_attachment->set_bone_name(bone_node->get_name());

	return bone_attachment;
}

FBXMeshIndex FBXDocument::_convert_mesh_to_fbx(Ref<FBXState> p_state, MeshInstance3D *p_mesh_instance) {
	ERR_FAIL_NULL_V(p_mesh_instance, -1);
	if (p_mesh_instance->get_mesh().is_null()) {
		return -1;
	}

	Ref<Mesh> import_mesh = p_mesh_instance->get_mesh();
	Ref<ImporterMesh> current_mesh = _mesh_to_importer_mesh(import_mesh);
	Vector<float> blend_weights;
	int32_t blend_count = import_mesh->get_blend_shape_count();
	blend_weights.resize(blend_count);
	for (int32_t blend_i = 0; blend_i < blend_count; blend_i++) {
		blend_weights.write[blend_i] = 0.0f;
	}

	Ref<FBXMesh> gltf_mesh;
	gltf_mesh.instantiate();
	TypedArray<Material> instance_materials;
	for (int32_t surface_i = 0; surface_i < current_mesh->get_surface_count(); surface_i++) {
		Ref<Material> mat = current_mesh->get_surface_material(surface_i);
		if (p_mesh_instance->get_surface_override_material(surface_i).is_valid()) {
			mat = p_mesh_instance->get_surface_override_material(surface_i);
		}
		if (p_mesh_instance->get_material_override().is_valid()) {
			mat = p_mesh_instance->get_material_override();
		}
		instance_materials.append(mat);
	}
	gltf_mesh->set_instance_materials(instance_materials);
	gltf_mesh->set_mesh(current_mesh);
	gltf_mesh->set_blend_weights(blend_weights);
	FBXMeshIndex mesh_i = p_state->meshes.size();
	p_state->meshes.push_back(gltf_mesh);
	return mesh_i;
}

ImporterMeshInstance3D *FBXDocument::_generate_mesh_instance(Ref<FBXState> p_state, const FBXNodeIndex p_node_index) {
	Ref<FBXNode> gltf_node = p_state->nodes[p_node_index];

	ERR_FAIL_INDEX_V(gltf_node->mesh, p_state->meshes.size(), nullptr);

	ImporterMeshInstance3D *mi = memnew(ImporterMeshInstance3D);
	print_verbose("glTF: Creating mesh for: " + gltf_node->get_name());

	p_state->scene_mesh_instances.insert(p_node_index, mi);
	Ref<FBXMesh> mesh = p_state->meshes.write[gltf_node->mesh];
	if (mesh.is_null()) {
		return mi;
	}
	Ref<ImporterMesh> import_mesh = mesh->get_mesh();
	if (import_mesh.is_null()) {
		return mi;
	}
	mi->set_mesh(import_mesh);
	return mi;
}

Camera3D *FBXDocument::_generate_camera(Ref<FBXState> p_state, const FBXNodeIndex p_node_index) {
	Ref<FBXNode> gltf_node = p_state->nodes[p_node_index];

	ERR_FAIL_INDEX_V(gltf_node->camera, p_state->cameras.size(), nullptr);

	print_verbose("glTF: Creating camera for: " + gltf_node->get_name());

	Ref<FBXCamera> c = p_state->cameras[gltf_node->camera];
	return c->to_node();
}

FBXCameraIndex FBXDocument::_convert_camera(Ref<FBXState> p_state, Camera3D *p_camera) {
	print_verbose("glTF: Converting camera: " + p_camera->get_name());

	Ref<FBXCamera> c = FBXCamera::from_node(p_camera);
	FBXCameraIndex camera_index = p_state->cameras.size();
	p_state->cameras.push_back(c);
	return camera_index;
}

void FBXDocument::_convert_spatial(Ref<FBXState> p_state, Node3D *p_spatial, Ref<FBXNode> p_node) {
	Transform3D xform = p_spatial->get_transform();
	p_node->scale = xform.basis.get_scale();
	p_node->rotation = xform.basis.get_rotation_quaternion();
	p_node->position = xform.origin;
}

Node3D *FBXDocument::_generate_spatial(Ref<FBXState> p_state, const FBXNodeIndex p_node_index) {
	Ref<FBXNode> gltf_node = p_state->nodes[p_node_index];

	Node3D *spatial = memnew(Node3D);
	print_verbose("glTF: Converting spatial: " + gltf_node->get_name());

	return spatial;
}

void FBXDocument::_convert_scene_node(Ref<FBXState> p_state, Node *p_current, const FBXNodeIndex p_gltf_parent, const FBXNodeIndex p_gltf_root) {
	bool retflag = true;
	_check_visibility(p_current, retflag);
	if (retflag) {
		return;
	}
	Ref<FBXNode> gltf_node;
	gltf_node.instantiate();
	gltf_node->set_name(_gen_unique_name(p_state, p_current->get_name()));
	if (cast_to<Node3D>(p_current)) {
		Node3D *spatial = cast_to<Node3D>(p_current);
		_convert_spatial(p_state, spatial, gltf_node);
	}
	if (cast_to<MeshInstance3D>(p_current)) {
		MeshInstance3D *mi = cast_to<MeshInstance3D>(p_current);
		_convert_mesh_instance_to_fbx(mi, p_state, gltf_node);
	} else if (cast_to<BoneAttachment3D>(p_current)) {
		BoneAttachment3D *bone = cast_to<BoneAttachment3D>(p_current);
		_convert_bone_attachment_to_fbx(bone, p_state, p_gltf_parent, p_gltf_root, gltf_node);
		return;
	} else if (cast_to<Skeleton3D>(p_current)) {
		Skeleton3D *skel = cast_to<Skeleton3D>(p_current);
		_convert_skeleton_to_fbx(skel, p_state, p_gltf_parent, p_gltf_root, gltf_node);
		// We ignore the Godot Engine node that is the skeleton.
		return;
	} else if (cast_to<MultiMeshInstance3D>(p_current)) {
		MultiMeshInstance3D *multi = cast_to<MultiMeshInstance3D>(p_current);
		_convert_multi_mesh_instance_to_fbx(multi, p_gltf_parent, p_gltf_root, gltf_node, p_state);
#ifdef MODULE_CSG_ENABLED
	} else if (cast_to<CSGShape3D>(p_current)) {
		CSGShape3D *shape = cast_to<CSGShape3D>(p_current);
		if (shape->get_parent() && shape->is_root_shape()) {
			_convert_csg_shape_to_fbx(shape, p_gltf_parent, gltf_node, p_state);
		}
#endif // MODULE_CSG_ENABLED
#ifdef MODULE_GRIDMAP_ENABLED
	} else if (cast_to<GridMap>(p_current)) {
		GridMap *gridmap = Object::cast_to<GridMap>(p_current);
		_convert_grid_map_to_fbx(gridmap, p_gltf_parent, p_gltf_root, gltf_node, p_state);
#endif // MODULE_GRIDMAP_ENABLED
	} else if (cast_to<Camera3D>(p_current)) {
		Camera3D *camera = Object::cast_to<Camera3D>(p_current);
		_convert_camera_to_fbx(camera, p_state, gltf_node);
	} else if (cast_to<AnimationPlayer>(p_current)) {
		AnimationPlayer *animation_player = Object::cast_to<AnimationPlayer>(p_current);
		_convert_animation_player_to_fbx(animation_player, p_state, p_gltf_parent, p_gltf_root, gltf_node, p_current);
	}
	for (Ref<FBXDocumentExtension> ext : document_extensions) {
		ERR_CONTINUE(ext.is_null());
		ext->convert_scene_node(p_state, gltf_node, p_current);
	}
	FBXNodeIndex current_node_i = p_state->nodes.size();
	FBXNodeIndex gltf_root = p_gltf_root;
	if (gltf_root == -1) {
		gltf_root = current_node_i;
		p_state->root_nodes.push_back(gltf_root);
	}
	_create_fbx_node(p_state, p_current, current_node_i, p_gltf_parent, gltf_root, gltf_node);
	for (int node_i = 0; node_i < p_current->get_child_count(); node_i++) {
		_convert_scene_node(p_state, p_current->get_child(node_i), current_node_i, gltf_root);
	}
}

#ifdef MODULE_CSG_ENABLED
void FBXDocument::_convert_csg_shape_to_fbx(CSGShape3D *p_current, FBXNodeIndex p_gltf_parent, Ref<FBXNode> p_gltf_node, Ref<FBXState> p_state) {
	CSGShape3D *csg = p_current;
	csg->call("_update_shape");
	Array meshes = csg->get_meshes();
	if (meshes.size() != 2) {
		return;
	}

	Ref<ImporterMesh> mesh;
	mesh.instantiate();
	{
		Ref<Mesh> csg_mesh = csg->get_meshes()[1];

		for (int32_t surface_i = 0; surface_i < csg_mesh->get_surface_count(); surface_i++) {
			Array array = csg_mesh->surface_get_arrays(surface_i);
			Ref<Material> mat = csg_mesh->surface_get_material(surface_i);
			String mat_name;
			if (mat.is_valid()) {
				mat_name = mat->get_name();
			} else {
				// Assign default material when no material is assigned.
				mat = Ref<StandardMaterial3D>(memnew(StandardMaterial3D));
			}
			mesh->add_surface(csg_mesh->surface_get_primitive_type(surface_i),
					array, csg_mesh->surface_get_blend_shape_arrays(surface_i), csg_mesh->surface_get_lods(surface_i), mat,
					mat_name, csg_mesh->surface_get_format(surface_i));
		}
	}

	Ref<FBXMesh> gltf_mesh;
	gltf_mesh.instantiate();
	gltf_mesh->set_mesh(mesh);
	FBXMeshIndex mesh_i = p_state->meshes.size();
	p_state->meshes.push_back(gltf_mesh);
	p_gltf_node->mesh = mesh_i;
	p_gltf_node->xform = csg->get_meshes()[0];
	p_gltf_node->set_name(_gen_unique_name(p_state, csg->get_name()));
}
#endif // MODULE_CSG_ENABLED

void FBXDocument::_create_fbx_node(Ref<FBXState> p_state, Node *p_scene_parent, FBXNodeIndex p_current_node_i,
		FBXNodeIndex p_parent_node_index, FBXNodeIndex p_root_gltf_node, Ref<FBXNode> p_gltf_node) {
	p_state->scene_nodes.insert(p_current_node_i, p_scene_parent);
	p_state->nodes.push_back(p_gltf_node);
	ERR_FAIL_COND(p_current_node_i == p_parent_node_index);
	p_state->nodes.write[p_current_node_i]->parent = p_parent_node_index;
	if (p_parent_node_index == -1) {
		return;
	}
	p_state->nodes.write[p_parent_node_index]->children.push_back(p_current_node_i);
}

void FBXDocument::_convert_animation_player_to_fbx(AnimationPlayer *p_animation_player, Ref<FBXState> p_state, FBXNodeIndex p_gltf_current, FBXNodeIndex p_gltf_root_index, Ref<FBXNode> p_gltf_node, Node *p_scene_parent) {
	ERR_FAIL_COND(!p_animation_player);
	p_state->animation_players.push_back(p_animation_player);
	print_verbose(String("glTF: Converting animation player: ") + p_animation_player->get_name());
}

void FBXDocument::_check_visibility(Node *p_node, bool &r_retflag) {
	r_retflag = true;
	Node3D *spatial = Object::cast_to<Node3D>(p_node);
	Node2D *node_2d = Object::cast_to<Node2D>(p_node);
	if (node_2d && !node_2d->is_visible()) {
		return;
	}
	if (spatial && !spatial->is_visible()) {
		return;
	}
	r_retflag = false;
}

void FBXDocument::_convert_camera_to_fbx(Camera3D *camera, Ref<FBXState> p_state, Ref<FBXNode> p_gltf_node) {
	ERR_FAIL_COND(!camera);
	FBXCameraIndex camera_index = _convert_camera(p_state, camera);
	if (camera_index != -1) {
		p_gltf_node->camera = camera_index;
	}
}

#ifdef MODULE_GRIDMAP_ENABLED
void FBXDocument::_convert_grid_map_to_fbx(GridMap *p_grid_map, FBXNodeIndex p_parent_node_index, FBXNodeIndex p_root_node_index, Ref<FBXNode> p_gltf_node, Ref<FBXState> p_state) {
	Array cells = p_grid_map->get_used_cells();
	for (int32_t k = 0; k < cells.size(); k++) {
		FBXNode *new_gltf_node = memnew(FBXNode);
		p_gltf_node->children.push_back(p_state->nodes.size());
		p_state->nodes.push_back(new_gltf_node);
		Vector3 cell_location = cells[k];
		int32_t cell = p_grid_map->get_cell_item(
				Vector3(cell_location.x, cell_location.y, cell_location.z));
		Transform3D cell_xform;
		cell_xform.basis = p_grid_map->get_basis_with_orthogonal_index(
				p_grid_map->get_cell_item_orientation(
						Vector3(cell_location.x, cell_location.y, cell_location.z)));
		cell_xform.basis.scale(Vector3(p_grid_map->get_cell_scale(),
				p_grid_map->get_cell_scale(),
				p_grid_map->get_cell_scale()));
		cell_xform.set_origin(p_grid_map->map_to_local(
				Vector3(cell_location.x, cell_location.y, cell_location.z)));
		Ref<FBXMesh> gltf_mesh;
		gltf_mesh.instantiate();
		gltf_mesh->set_mesh(_mesh_to_importer_mesh(p_grid_map->get_mesh_library()->get_item_mesh(cell)));
		new_gltf_node->mesh = p_state->meshes.size();
		p_state->meshes.push_back(gltf_mesh);
		new_gltf_node->xform = cell_xform * p_grid_map->get_transform();
		new_gltf_node->set_name(_gen_unique_name(p_state, p_grid_map->get_mesh_library()->get_item_name(cell)));
	}
}
#endif // MODULE_GRIDMAP_ENABLED

void FBXDocument::_convert_multi_mesh_instance_to_fbx(
		MultiMeshInstance3D *p_multi_mesh_instance,
		FBXNodeIndex p_parent_node_index,
		FBXNodeIndex p_root_node_index,
		Ref<FBXNode> p_gltf_node, Ref<FBXState> p_state) {
	ERR_FAIL_COND(!p_multi_mesh_instance);
	Ref<MultiMesh> multi_mesh = p_multi_mesh_instance->get_multimesh();
	if (multi_mesh.is_null()) {
		return;
	}
	Ref<FBXMesh> gltf_mesh;
	gltf_mesh.instantiate();
	Ref<Mesh> mesh = multi_mesh->get_mesh();
	if (mesh.is_null()) {
		return;
	}
	gltf_mesh->set_name(multi_mesh->get_name());
	Ref<ImporterMesh> importer_mesh;
	importer_mesh.instantiate();
	Ref<ArrayMesh> array_mesh = multi_mesh->get_mesh();
	if (array_mesh.is_valid()) {
		importer_mesh->set_blend_shape_mode(array_mesh->get_blend_shape_mode());
		for (int32_t blend_i = 0; blend_i < array_mesh->get_blend_shape_count(); blend_i++) {
			importer_mesh->add_blend_shape(array_mesh->get_blend_shape_name(blend_i));
		}
	}
	for (int32_t surface_i = 0; surface_i < mesh->get_surface_count(); surface_i++) {
		Ref<Material> mat = mesh->surface_get_material(surface_i);
		String material_name;
		if (mat.is_valid()) {
			material_name = mat->get_name();
		}
		Array blend_arrays;
		if (array_mesh.is_valid()) {
			blend_arrays = array_mesh->surface_get_blend_shape_arrays(surface_i);
		}
		importer_mesh->add_surface(mesh->surface_get_primitive_type(surface_i), mesh->surface_get_arrays(surface_i),
				blend_arrays, mesh->surface_get_lods(surface_i), mat, material_name, mesh->surface_get_format(surface_i));
	}
	gltf_mesh->set_mesh(importer_mesh);
	FBXMeshIndex mesh_index = p_state->meshes.size();
	p_state->meshes.push_back(gltf_mesh);
	for (int32_t instance_i = 0; instance_i < multi_mesh->get_instance_count();
			instance_i++) {
		Transform3D transform;
		if (multi_mesh->get_transform_format() == MultiMesh::TRANSFORM_2D) {
			Transform2D xform_2d = multi_mesh->get_instance_transform_2d(instance_i);
			transform.origin =
					Vector3(xform_2d.get_origin().x, 0, xform_2d.get_origin().y);
			real_t rotation = xform_2d.get_rotation();
			Quaternion quaternion(Vector3(0, 1, 0), rotation);
			Size2 scale = xform_2d.get_scale();
			transform.basis.set_quaternion_scale(quaternion,
					Vector3(scale.x, 0, scale.y));
			transform = p_multi_mesh_instance->get_transform() * transform;
		} else if (multi_mesh->get_transform_format() == MultiMesh::TRANSFORM_3D) {
			transform = p_multi_mesh_instance->get_transform() *
					multi_mesh->get_instance_transform(instance_i);
		}
		Ref<FBXNode> new_gltf_node;
		new_gltf_node.instantiate();
		new_gltf_node->mesh = mesh_index;
		new_gltf_node->xform = transform;
		new_gltf_node->set_name(_gen_unique_name(p_state, p_multi_mesh_instance->get_name()));
		p_gltf_node->children.push_back(p_state->nodes.size());
		p_state->nodes.push_back(new_gltf_node);
	}
}

void FBXDocument::_convert_skeleton_to_fbx(Skeleton3D *p_skeleton3d, Ref<FBXState> p_state, FBXNodeIndex p_parent_node_index, FBXNodeIndex p_root_node_index, Ref<FBXNode> p_gltf_node) {
	Skeleton3D *skeleton = p_skeleton3d;
	Ref<FBXSkeleton> gltf_skeleton;
	gltf_skeleton.instantiate();
	// FBXSkeleton is only used to hold internal p_state data. It will not be written to the document.
	//
	gltf_skeleton->godot_skeleton = skeleton;
	FBXSkeletonIndex skeleton_i = p_state->skeletons.size();
	p_state->skeleton3d_to_gltf_skeleton[skeleton->get_instance_id()] = skeleton_i;
	p_state->skeletons.push_back(gltf_skeleton);

	BoneId bone_count = skeleton->get_bone_count();
	for (BoneId bone_i = 0; bone_i < bone_count; bone_i++) {
		Ref<FBXNode> joint_node;
		joint_node.instantiate();
		// Note that we cannot use _gen_unique_bone_name here, because glTF spec requires all node
		// names to be unique regardless of whether or not they are used as joints.
		joint_node->set_name(_gen_unique_name(p_state, skeleton->get_bone_name(bone_i)));
		Transform3D xform = skeleton->get_bone_pose(bone_i);
		joint_node->scale = xform.basis.get_scale();
		joint_node->rotation = xform.basis.get_rotation_quaternion();
		joint_node->position = xform.origin;
		joint_node->joint = true;
		FBXNodeIndex current_node_i = p_state->nodes.size();
		p_state->scene_nodes.insert(current_node_i, skeleton);
		p_state->nodes.push_back(joint_node);

		gltf_skeleton->joints.push_back(current_node_i);
		if (skeleton->get_bone_parent(bone_i) == -1) {
			gltf_skeleton->roots.push_back(current_node_i);
		}
		gltf_skeleton->godot_bone_node.insert(bone_i, current_node_i);
	}
	for (BoneId bone_i = 0; bone_i < bone_count; bone_i++) {
		FBXNodeIndex current_node_i = gltf_skeleton->godot_bone_node[bone_i];
		BoneId parent_bone_id = skeleton->get_bone_parent(bone_i);
		if (parent_bone_id == -1) {
			if (p_parent_node_index != -1) {
				p_state->nodes.write[current_node_i]->parent = p_parent_node_index;
				p_state->nodes.write[p_parent_node_index]->children.push_back(current_node_i);
			}
		} else {
			FBXNodeIndex parent_node_i = gltf_skeleton->godot_bone_node[parent_bone_id];
			p_state->nodes.write[current_node_i]->parent = parent_node_i;
			p_state->nodes.write[parent_node_i]->children.push_back(current_node_i);
		}
	}
	// Remove placeholder skeleton3d node by not creating the gltf node
	// Skins are per mesh
	for (int node_i = 0; node_i < skeleton->get_child_count(); node_i++) {
		_convert_scene_node(p_state, skeleton->get_child(node_i), p_parent_node_index, p_root_node_index);
	}
}

void FBXDocument::_convert_bone_attachment_to_fbx(BoneAttachment3D *p_bone_attachment, Ref<FBXState> p_state, FBXNodeIndex p_parent_node_index, FBXNodeIndex p_root_node_index, Ref<FBXNode> p_gltf_node) {
	Skeleton3D *skeleton;
	// Note that relative transforms to external skeletons and pose overrides are not supported.
	if (p_bone_attachment->get_use_external_skeleton()) {
		skeleton = cast_to<Skeleton3D>(p_bone_attachment->get_node_or_null(p_bone_attachment->get_external_skeleton()));
	} else {
		skeleton = cast_to<Skeleton3D>(p_bone_attachment->get_parent());
	}
	FBXSkeletonIndex skel_gltf_i = -1;
	if (skeleton != nullptr && p_state->skeleton3d_to_gltf_skeleton.has(skeleton->get_instance_id())) {
		skel_gltf_i = p_state->skeleton3d_to_gltf_skeleton[skeleton->get_instance_id()];
	}
	int bone_idx = -1;
	if (skeleton != nullptr) {
		bone_idx = p_bone_attachment->get_bone_idx();
		if (bone_idx == -1) {
			bone_idx = skeleton->find_bone(p_bone_attachment->get_bone_name());
		}
	}
	FBXNodeIndex par_node_index = p_parent_node_index;
	if (skeleton != nullptr && bone_idx != -1 && skel_gltf_i != -1) {
		Ref<FBXSkeleton> gltf_skeleton = p_state->skeletons.write[skel_gltf_i];
		gltf_skeleton->bone_attachments.push_back(p_bone_attachment);
		par_node_index = gltf_skeleton->joints[bone_idx];
	}

	for (int node_i = 0; node_i < p_bone_attachment->get_child_count(); node_i++) {
		_convert_scene_node(p_state, p_bone_attachment->get_child(node_i), par_node_index, p_root_node_index);
	}
}

void FBXDocument::_convert_mesh_instance_to_fbx(MeshInstance3D *p_scene_parent, Ref<FBXState> p_state, Ref<FBXNode> p_gltf_node) {
	FBXMeshIndex gltf_mesh_index = _convert_mesh_to_fbx(p_state, p_scene_parent);
	if (gltf_mesh_index != -1) {
		p_gltf_node->mesh = gltf_mesh_index;
	}
}

void FBXDocument::_generate_scene_node(Ref<FBXState> p_state, const FBXNodeIndex p_node_index, Node *p_scene_parent, Node *p_scene_root) {
	Ref<FBXNode> gltf_node = p_state->nodes[p_node_index];

	if (gltf_node->skeleton >= 0) {
		_generate_skeleton_bone_node(p_state, p_node_index, p_scene_parent, p_scene_root);
		return;
	}

	Node3D *current_node = nullptr;

	// Is our parent a skeleton
	Skeleton3D *active_skeleton = Object::cast_to<Skeleton3D>(p_scene_parent);

	const bool non_bone_parented_to_skeleton = active_skeleton;

	// skinned meshes must not be placed in a bone attachment.
	if (non_bone_parented_to_skeleton && gltf_node->skin < 0) {
		// Bone Attachment - Parent Case
		BoneAttachment3D *bone_attachment = _generate_bone_attachment(p_state, active_skeleton, p_node_index, gltf_node->parent);

		p_scene_parent->add_child(bone_attachment, true);
		bone_attachment->set_owner(p_scene_root);

		// There is no gltf_node that represent this, so just directly create a unique name
		bone_attachment->set_name(gltf_node->get_name());

		// We change the scene_parent to our bone attachment now. We do not set current_node because we want to make the node
		// and attach it to the bone_attachment
		p_scene_parent = bone_attachment;
	}
	// Check if any FBXDocumentExtension classes want to generate a node for us.
	for (Ref<FBXDocumentExtension> ext : document_extensions) {
		ERR_CONTINUE(ext.is_null());
		current_node = ext->generate_scene_node(p_state, gltf_node, p_scene_parent);
		if (current_node) {
			break;
		}
	}
	// If none of our FBXDocumentExtension classes generated us a node, we generate one.
	if (!current_node) {
		if (gltf_node->skin >= 0 && gltf_node->mesh >= 0 && !gltf_node->children.is_empty()) {
			current_node = _generate_spatial(p_state, p_node_index);
			Node3D *mesh_inst = _generate_mesh_instance(p_state, p_node_index);
			mesh_inst->set_name(gltf_node->get_name());

			current_node->add_child(mesh_inst, true);
		} else if (gltf_node->mesh >= 0) {
			current_node = _generate_mesh_instance(p_state, p_node_index);
		} else if (gltf_node->camera >= 0) {
			current_node = _generate_camera(p_state, p_node_index);
		} else {
			current_node = _generate_spatial(p_state, p_node_index);
		}
	}
	// Add the node we generated and set the owner to the scene root.
	p_scene_parent->add_child(current_node, true);
	if (current_node != p_scene_root) {
		Array args;
		args.append(p_scene_root);
		current_node->propagate_call(StringName("set_owner"), args);
	}
	current_node->set_transform(gltf_node->xform);
	current_node->set_name(gltf_node->get_name());

	p_state->scene_nodes.insert(p_node_index, current_node);
	for (int i = 0; i < gltf_node->children.size(); ++i) {
		_generate_scene_node(p_state, gltf_node->children[i], current_node, p_scene_root);
	}
}

void FBXDocument::_generate_skeleton_bone_node(Ref<FBXState> p_state, const FBXNodeIndex p_node_index, Node *p_scene_parent, Node *p_scene_root) {
	Ref<FBXNode> gltf_node = p_state->nodes[p_node_index];

	Node3D *current_node = nullptr;

	Skeleton3D *skeleton = p_state->skeletons[gltf_node->skeleton]->godot_skeleton;
	// In this case, this node is already a bone in skeleton.
	const bool is_skinned_mesh = (gltf_node->skin >= 0 && gltf_node->mesh >= 0);
	const bool requires_extra_node = (gltf_node->mesh >= 0 || gltf_node->camera >= 0);

	Skeleton3D *active_skeleton = Object::cast_to<Skeleton3D>(p_scene_parent);
	if (active_skeleton != skeleton) {
		if (active_skeleton) {
			// Should no longer be possible.
			ERR_PRINT(vformat("glTF: Generating scene detected direct parented Skeletons at node %d", p_node_index));
			BoneAttachment3D *bone_attachment = _generate_bone_attachment(p_state, active_skeleton, p_node_index, gltf_node->parent);
			p_scene_parent->add_child(bone_attachment, true);
			bone_attachment->set_owner(p_scene_root);
			// There is no gltf_node that represent this, so just directly create a unique name
			bone_attachment->set_name(_gen_unique_name(p_state, "BoneAttachment3D"));
			// We change the scene_parent to our bone attachment now. We do not set current_node because we want to make the node
			// and attach it to the bone_attachment
			p_scene_parent = bone_attachment;
		}
		if (skeleton->get_parent() == nullptr) {
			p_scene_parent->add_child(skeleton, true);
			skeleton->set_owner(p_scene_root);
		}
	}

	active_skeleton = skeleton;
	current_node = active_skeleton;

	if (requires_extra_node) {
		current_node = nullptr;
		// skinned meshes must not be placed in a bone attachment.
		if (!is_skinned_mesh) {
			// Bone Attachment - Same Node Case
			BoneAttachment3D *bone_attachment = _generate_bone_attachment(p_state, active_skeleton, p_node_index, p_node_index);

			p_scene_parent->add_child(bone_attachment, true);
			bone_attachment->set_owner(p_scene_root);

			// There is no gltf_node that represent this, so just directly create a unique name
			bone_attachment->set_name(gltf_node->get_name());

			// We change the scene_parent to our bone attachment now. We do not set current_node because we want to make the node
			// and attach it to the bone_attachment
			p_scene_parent = bone_attachment;
		}
		// Check if any FBXDocumentExtension classes want to generate a node for us.
		for (Ref<FBXDocumentExtension> ext : document_extensions) {
			ERR_CONTINUE(ext.is_null());
			current_node = ext->generate_scene_node(p_state, gltf_node, p_scene_parent);
			if (current_node) {
				break;
			}
		}
		// If none of our FBXDocumentExtension classes generated us a node, we generate one.
		if (!current_node) {
			if (gltf_node->mesh >= 0) {
				current_node = _generate_mesh_instance(p_state, p_node_index);
			} else if (gltf_node->camera >= 0) {
				current_node = _generate_camera(p_state, p_node_index);
			} else {
				current_node = _generate_spatial(p_state, p_node_index);
			}
		}
		// Add the node we generated and set the owner to the scene root.
		p_scene_parent->add_child(current_node, true);
		if (current_node != p_scene_root) {
			Array args;
			args.append(p_scene_root);
			current_node->propagate_call(StringName("set_owner"), args);
		}
		// Do not set transform here. Transform is already applied to our bone.
		current_node->set_name(gltf_node->get_name());
	}

	p_state->scene_nodes.insert(p_node_index, current_node);

	for (int i = 0; i < gltf_node->children.size(); ++i) {
		_generate_scene_node(p_state, gltf_node->children[i], active_skeleton, p_scene_root);
	}
}

template <class T>
struct SceneFormatImporterGLTFInterpolate {
	T lerp(const T &a, const T &b, float c) const {
		return a + (b - a) * c;
	}

	T catmull_rom(const T &p0, const T &p1, const T &p2, const T &p3, float t) {
		const float t2 = t * t;
		const float t3 = t2 * t;

		return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
	}

	T bezier(T start, T control_1, T control_2, T end, float t) {
		/* Formula from Wikipedia article on Bezier curves. */
		const real_t omt = (1.0 - t);
		const real_t omt2 = omt * omt;
		const real_t omt3 = omt2 * omt;
		const real_t t2 = t * t;
		const real_t t3 = t2 * t;

		return start * omt3 + control_1 * omt2 * t * 3.0 + control_2 * omt * t2 * 3.0 + end * t3;
	}
};

// thank you for existing, partial specialization
template <>
struct SceneFormatImporterGLTFInterpolate<Quaternion> {
	Quaternion lerp(const Quaternion &a, const Quaternion &b, const float c) const {
		ERR_FAIL_COND_V_MSG(!a.is_normalized(), Quaternion(), "The quaternion \"a\" must be normalized.");
		ERR_FAIL_COND_V_MSG(!b.is_normalized(), Quaternion(), "The quaternion \"b\" must be normalized.");

		return a.slerp(b, c).normalized();
	}

	Quaternion catmull_rom(const Quaternion &p0, const Quaternion &p1, const Quaternion &p2, const Quaternion &p3, const float c) {
		ERR_FAIL_COND_V_MSG(!p1.is_normalized(), Quaternion(), "The quaternion \"p1\" must be normalized.");
		ERR_FAIL_COND_V_MSG(!p2.is_normalized(), Quaternion(), "The quaternion \"p2\" must be normalized.");

		return p1.slerp(p2, c).normalized();
	}

	Quaternion bezier(const Quaternion start, const Quaternion control_1, const Quaternion control_2, const Quaternion end, const float t) {
		ERR_FAIL_COND_V_MSG(!start.is_normalized(), Quaternion(), "The start quaternion must be normalized.");
		ERR_FAIL_COND_V_MSG(!end.is_normalized(), Quaternion(), "The end quaternion must be normalized.");

		return start.slerp(end, t).normalized();
	}
};

template <class T>
T FBXDocument::_interpolate_track(const Vector<real_t> &p_times, const Vector<T> &p_values, const float p_time, const FBXAnimation::Interpolation p_interp) {
	ERR_FAIL_COND_V(!p_values.size(), T());
	if (p_times.size() != (p_values.size() / (p_interp == FBXAnimation::INTERP_CUBIC_SPLINE ? 3 : 1))) {
		ERR_PRINT_ONCE("The interpolated values are not corresponding to its times.");
		return p_values[0];
	}
	//could use binary search, worth it?
	int idx = -1;
	for (int i = 0; i < p_times.size(); i++) {
		if (p_times[i] > p_time) {
			break;
		}
		idx++;
	}

	SceneFormatImporterGLTFInterpolate<T> interp;

	switch (p_interp) {
		case FBXAnimation::INTERP_LINEAR: {
			if (idx == -1) {
				return p_values[0];
			} else if (idx >= p_times.size() - 1) {
				return p_values[p_times.size() - 1];
			}

			const float c = (p_time - p_times[idx]) / (p_times[idx + 1] - p_times[idx]);

			return interp.lerp(p_values[idx], p_values[idx + 1], c);
		} break;
		case FBXAnimation::INTERP_STEP: {
			if (idx == -1) {
				return p_values[0];
			} else if (idx >= p_times.size() - 1) {
				return p_values[p_times.size() - 1];
			}

			return p_values[idx];
		} break;
		case FBXAnimation::INTERP_CATMULLROMSPLINE: {
			if (idx == -1) {
				return p_values[1];
			} else if (idx >= p_times.size() - 1) {
				return p_values[1 + p_times.size() - 1];
			}

			const float c = (p_time - p_times[idx]) / (p_times[idx + 1] - p_times[idx]);

			return interp.catmull_rom(p_values[idx - 1], p_values[idx], p_values[idx + 1], p_values[idx + 3], c);
		} break;
		case FBXAnimation::INTERP_CUBIC_SPLINE: {
			if (idx == -1) {
				return p_values[1];
			} else if (idx >= p_times.size() - 1) {
				return p_values[(p_times.size() - 1) * 3 + 1];
			}

			const float c = (p_time - p_times[idx]) / (p_times[idx + 1] - p_times[idx]);

			const T from = p_values[idx * 3 + 1];
			const T c1 = from + p_values[idx * 3 + 2];
			const T to = p_values[idx * 3 + 4];
			const T c2 = to + p_values[idx * 3 + 3];

			return interp.bezier(from, c1, c2, to, c);
		} break;
	}

	ERR_FAIL_V(p_values[0]);
}

void FBXDocument::_import_animation(Ref<FBXState> p_state, AnimationPlayer *p_animation_player, const FBXAnimationIndex p_index, const float p_bake_fps, const bool p_trimming, const bool p_remove_immutable_tracks) {
	Ref<FBXAnimation> anim = p_state->animations[p_index];

	String anim_name = anim->get_name();
	if (anim_name.is_empty()) {
		// No node represent these, and they are not in the hierarchy, so just make a unique name
		anim_name = _gen_unique_name(p_state, "Animation");
	}

	Ref<Animation> animation;
	animation.instantiate();
	animation->set_name(anim_name);

	if (anim->get_loop()) {
		animation->set_loop_mode(Animation::LOOP_LINEAR);
	}

	double anim_start = p_trimming ? INFINITY : 0.0;
	double anim_end = 0.0;

	for (const KeyValue<int, FBXAnimation::Track> &track_i : anim->get_tracks()) {
		const FBXAnimation::Track &track = track_i.value;
		//need to find the path: for skeletons, weight tracks will affect the mesh
		NodePath node_path;
		//for skeletons, transform tracks always affect bones
		NodePath transform_node_path;
		//for meshes, especially skinned meshes, there are cases where it will be added as a child
		NodePath mesh_instance_node_path;

		FBXNodeIndex node_index = track_i.key;

		const Ref<FBXNode> gltf_node = p_state->nodes[track_i.key];

		Node *root = p_animation_player->get_parent();
		ERR_FAIL_COND(root == nullptr);
		HashMap<FBXNodeIndex, Node *>::Iterator node_element = p_state->scene_nodes.find(node_index);
		ERR_CONTINUE_MSG(!node_element, vformat("Unable to find node %d for animation.", node_index));
		node_path = root->get_path_to(node_element->value);
		HashMap<FBXNodeIndex, ImporterMeshInstance3D *>::Iterator mesh_instance_element = p_state->scene_mesh_instances.find(node_index);
		if (mesh_instance_element) {
			mesh_instance_node_path = root->get_path_to(mesh_instance_element->value);
		} else {
			mesh_instance_node_path = node_path;
		}

		if (gltf_node->skeleton >= 0) {
			const Skeleton3D *sk = p_state->skeletons[gltf_node->skeleton]->godot_skeleton;
			ERR_FAIL_COND(sk == nullptr);

			const String path = p_animation_player->get_parent()->get_path_to(sk);
			const String bone = gltf_node->get_name();
			transform_node_path = path + ":" + bone;
		} else {
			transform_node_path = node_path;
		}

		if (p_trimming) {
			for (int i = 0; i < track.rotation_track.times.size(); i++) {
				anim_start = MIN(anim_start, track.rotation_track.times[i]);
				anim_end = MAX(anim_end, track.rotation_track.times[i]);
			}
			for (int i = 0; i < track.position_track.times.size(); i++) {
				anim_start = MIN(anim_start, track.position_track.times[i]);
				anim_end = MAX(anim_end, track.position_track.times[i]);
			}
			for (int i = 0; i < track.scale_track.times.size(); i++) {
				anim_start = MIN(anim_start, track.scale_track.times[i]);
				anim_end = MAX(anim_end, track.scale_track.times[i]);
			}
			for (int i = 0; i < track.weight_tracks.size(); i++) {
				for (int j = 0; j < track.weight_tracks[i].times.size(); j++) {
					anim_start = MIN(anim_start, track.weight_tracks[i].times[j]);
					anim_end = MAX(anim_end, track.weight_tracks[i].times[j]);
				}
			}
		} else {
			// If you don't use trimming and the first key time is not at 0.0, fake keys will be inserted.
			for (int i = 0; i < track.rotation_track.times.size(); i++) {
				anim_end = MAX(anim_end, track.rotation_track.times[i]);
			}
			for (int i = 0; i < track.position_track.times.size(); i++) {
				anim_end = MAX(anim_end, track.position_track.times[i]);
			}
			for (int i = 0; i < track.scale_track.times.size(); i++) {
				anim_end = MAX(anim_end, track.scale_track.times[i]);
			}
			for (int i = 0; i < track.weight_tracks.size(); i++) {
				for (int j = 0; j < track.weight_tracks[i].times.size(); j++) {
					anim_end = MAX(anim_end, track.weight_tracks[i].times[j]);
				}
			}
		}

		// Animated TRS properties will not affect a skinned mesh.
		const bool transform_affects_skinned_mesh_instance = gltf_node->skeleton < 0 && gltf_node->skin >= 0;
		if ((track.rotation_track.values.size() || track.position_track.values.size() || track.scale_track.values.size()) && !transform_affects_skinned_mesh_instance) {
			//make transform track
			int base_idx = animation->get_track_count();
			int position_idx = -1;
			int rotation_idx = -1;
			int scale_idx = -1;

			if (track.position_track.values.size()) {
				bool is_default = true; //discard the track if all it contains is default values
				if (p_remove_immutable_tracks) {
					Vector3 base_pos = p_state->nodes[track_i.key]->position;
					for (int i = 0; i < track.position_track.times.size(); i++) {
						Vector3 value = track.position_track.values[track.position_track.interpolation == FBXAnimation::INTERP_CUBIC_SPLINE ? (1 + i * 3) : i];
						if (!value.is_equal_approx(base_pos)) {
							is_default = false;
							break;
						}
					}
				}
				if (!p_remove_immutable_tracks || !is_default) {
					position_idx = base_idx;
					animation->add_track(Animation::TYPE_POSITION_3D);
					animation->track_set_path(position_idx, transform_node_path);
					animation->track_set_imported(position_idx, true); //helps merging later
					base_idx++;
				}
			}
			if (track.rotation_track.values.size()) {
				bool is_default = true; //discard the track if all it contains is default values
				if (p_remove_immutable_tracks) {
					Quaternion base_rot = p_state->nodes[track_i.key]->rotation.normalized();
					for (int i = 0; i < track.rotation_track.times.size(); i++) {
						Quaternion value = track.rotation_track.values[track.rotation_track.interpolation == FBXAnimation::INTERP_CUBIC_SPLINE ? (1 + i * 3) : i].normalized();
						if (!value.is_equal_approx(base_rot)) {
							is_default = false;
							break;
						}
					}
				}
				if (!p_remove_immutable_tracks || !is_default) {
					rotation_idx = base_idx;
					animation->add_track(Animation::TYPE_ROTATION_3D);
					animation->track_set_path(rotation_idx, transform_node_path);
					animation->track_set_imported(rotation_idx, true); //helps merging later
					base_idx++;
				}
			}
			if (track.scale_track.values.size()) {
				bool is_default = true; //discard the track if all it contains is default values
				if (p_remove_immutable_tracks) {
					Vector3 base_scale = p_state->nodes[track_i.key]->scale;
					for (int i = 0; i < track.scale_track.times.size(); i++) {
						Vector3 value = track.scale_track.values[track.scale_track.interpolation == FBXAnimation::INTERP_CUBIC_SPLINE ? (1 + i * 3) : i];
						if (!value.is_equal_approx(base_scale)) {
							is_default = false;
							break;
						}
					}
				}
				if (!p_remove_immutable_tracks || !is_default) {
					scale_idx = base_idx;
					animation->add_track(Animation::TYPE_SCALE_3D);
					animation->track_set_path(scale_idx, transform_node_path);
					animation->track_set_imported(scale_idx, true); //helps merging later
					base_idx++;
				}
			}

			const double increment = 1.0 / p_bake_fps;
			double time = anim_start;

			Vector3 base_pos;
			Quaternion base_rot;
			Vector3 base_scale = Vector3(1, 1, 1);

			if (rotation_idx == -1) {
				base_rot = p_state->nodes[track_i.key]->rotation.normalized();
			}

			if (position_idx == -1) {
				base_pos = p_state->nodes[track_i.key]->position;
			}

			if (scale_idx == -1) {
				base_scale = p_state->nodes[track_i.key]->scale;
			}

			bool last = false;
			while (true) {
				Vector3 pos = base_pos;
				Quaternion rot = base_rot;
				Vector3 scale = base_scale;

				if (position_idx >= 0) {
					pos = _interpolate_track<Vector3>(track.position_track.times, track.position_track.values, time, track.position_track.interpolation);
					animation->position_track_insert_key(position_idx, time - anim_start, pos);
				}

				if (rotation_idx >= 0) {
					rot = _interpolate_track<Quaternion>(track.rotation_track.times, track.rotation_track.values, time, track.rotation_track.interpolation);
					animation->rotation_track_insert_key(rotation_idx, time - anim_start, rot);
				}

				if (scale_idx >= 0) {
					scale = _interpolate_track<Vector3>(track.scale_track.times, track.scale_track.values, time, track.scale_track.interpolation);
					animation->scale_track_insert_key(scale_idx, time - anim_start, scale);
				}

				if (last) {
					break;
				}
				time += increment;
				if (time >= anim_end) {
					last = true;
					time = anim_end;
				}
			}
		}

		for (int i = 0; i < track.weight_tracks.size(); i++) {
			ERR_CONTINUE(gltf_node->mesh < 0 || gltf_node->mesh >= p_state->meshes.size());
			Ref<FBXMesh> mesh = p_state->meshes[gltf_node->mesh];
			ERR_CONTINUE(mesh.is_null());
			ERR_CONTINUE(mesh->get_mesh().is_null());
			ERR_CONTINUE(mesh->get_mesh()->get_mesh().is_null());

			const String blend_path = String(mesh_instance_node_path) + ":" + String(mesh->get_mesh()->get_blend_shape_name(i));

			const int track_idx = animation->get_track_count();
			animation->add_track(Animation::TYPE_BLEND_SHAPE);
			animation->track_set_path(track_idx, blend_path);
			animation->track_set_imported(track_idx, true); //helps merging later

			// Only LINEAR and STEP (NEAREST) can be supported out of the box by Godot's Animation,
			// the other modes have to be baked.
			FBXAnimation::Interpolation gltf_interp = track.weight_tracks[i].interpolation;
			if (gltf_interp == FBXAnimation::INTERP_LINEAR || gltf_interp == FBXAnimation::INTERP_STEP) {
				animation->track_set_interpolation_type(track_idx, gltf_interp == FBXAnimation::INTERP_STEP ? Animation::INTERPOLATION_NEAREST : Animation::INTERPOLATION_LINEAR);
				for (int j = 0; j < track.weight_tracks[i].times.size(); j++) {
					const float t = track.weight_tracks[i].times[j];
					const float attribs = track.weight_tracks[i].values[j];
					animation->blend_shape_track_insert_key(track_idx, t, attribs);
				}
			} else {
				// CATMULLROMSPLINE or CUBIC_SPLINE have to be baked, apologies.
				const double increment = 1.0 / p_bake_fps;
				double time = 0.0;
				bool last = false;
				while (true) {
					real_t blend = _interpolate_track<real_t>(track.weight_tracks[i].times, track.weight_tracks[i].values, time, gltf_interp);
					animation->blend_shape_track_insert_key(track_idx, time - anim_start, blend);
					if (last) {
						break;
					}
					time += increment;
					if (time >= anim_end) {
						last = true;
						time = anim_end;
					}
				}
			}
		}
	}

	animation->set_length(anim_end - anim_start);

	Ref<AnimationLibrary> library;
	if (!p_animation_player->has_animation_library("")) {
		library.instantiate();
		p_animation_player->add_animation_library("", library);
	} else {
		library = p_animation_player->get_animation_library("");
	}
	library->add_animation(anim_name, animation);
}

void FBXDocument::_convert_mesh_instances(Ref<FBXState> p_state) {
	for (FBXNodeIndex mi_node_i = 0; mi_node_i < p_state->nodes.size(); ++mi_node_i) {
		Ref<FBXNode> node = p_state->nodes[mi_node_i];

		if (node->mesh < 0) {
			continue;
		}
		HashMap<FBXNodeIndex, Node *>::Iterator mi_element = p_state->scene_nodes.find(mi_node_i);
		if (!mi_element) {
			continue;
		}
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(mi_element->value);
		if (!mi) {
			continue;
		}
		Transform3D mi_xform = mi->get_transform();
		node->scale = mi_xform.basis.get_scale();
		node->rotation = mi_xform.basis.get_rotation_quaternion();
		node->position = mi_xform.origin;

		Node *skel_node = mi->get_node_or_null(mi->get_skeleton_path());
		Skeleton3D *godot_skeleton = Object::cast_to<Skeleton3D>(skel_node);
		if (!godot_skeleton || godot_skeleton->get_bone_count() == 0) {
			continue;
		}
		// At this point in the code, we know we have a Skeleton3D with at least one bone.
		Ref<Skin> skin = mi->get_skin();
		Ref<FBXSkin> gltf_skin;
		gltf_skin.instantiate();
		Array json_joints;
		if (p_state->skeleton3d_to_gltf_skeleton.has(godot_skeleton->get_instance_id())) {
			// This is a skinned mesh. If the mesh has no ARRAY_WEIGHTS or ARRAY_BONES, it will be invisible.
			const FBXSkeletonIndex skeleton_gltf_i = p_state->skeleton3d_to_gltf_skeleton[godot_skeleton->get_instance_id()];
			Ref<FBXSkeleton> gltf_skeleton = p_state->skeletons[skeleton_gltf_i];
			int bone_cnt = godot_skeleton->get_bone_count();
			ERR_FAIL_COND(bone_cnt != gltf_skeleton->joints.size());

			ObjectID gltf_skin_key;
			if (skin.is_valid()) {
				gltf_skin_key = skin->get_instance_id();
			}
			ObjectID gltf_skel_key = godot_skeleton->get_instance_id();
			FBXSkinIndex skin_gltf_i = -1;
			FBXNodeIndex root_gltf_i = -1;
			if (!gltf_skeleton->roots.is_empty()) {
				root_gltf_i = gltf_skeleton->roots[0];
			}
			if (p_state->skin_and_skeleton3d_to_gltf_skin.has(gltf_skin_key) && p_state->skin_and_skeleton3d_to_gltf_skin[gltf_skin_key].has(gltf_skel_key)) {
				skin_gltf_i = p_state->skin_and_skeleton3d_to_gltf_skin[gltf_skin_key][gltf_skel_key];
			} else {
				if (skin.is_null()) {
					// Note that gltf_skin_key should remain null, so these can share a reference.
					skin = godot_skeleton->create_skin_from_rest_transforms();
				}
				gltf_skin.instantiate();
				gltf_skin->godot_skin = skin;
				gltf_skin->set_name(skin->get_name());
				gltf_skin->skeleton = skeleton_gltf_i;
				gltf_skin->skin_root = root_gltf_i;
				//gltf_state->godot_to_gltf_node[skel_node]
				HashMap<StringName, int> bone_name_to_idx;
				for (int bone_i = 0; bone_i < bone_cnt; bone_i++) {
					bone_name_to_idx[godot_skeleton->get_bone_name(bone_i)] = bone_i;
				}
				for (int bind_i = 0, cnt = skin->get_bind_count(); bind_i < cnt; bind_i++) {
					int bone_i = skin->get_bind_bone(bind_i);
					Transform3D bind_pose = skin->get_bind_pose(bind_i);
					StringName bind_name = skin->get_bind_name(bind_i);
					if (bind_name != StringName()) {
						bone_i = bone_name_to_idx[bind_name];
					}
					ERR_CONTINUE(bone_i < 0 || bone_i >= bone_cnt);
					if (bind_name == StringName()) {
						bind_name = godot_skeleton->get_bone_name(bone_i);
					}
					FBXNodeIndex skeleton_bone_i = gltf_skeleton->joints[bone_i];
					gltf_skin->joints_original.push_back(skeleton_bone_i);
					gltf_skin->joints.push_back(skeleton_bone_i);
					gltf_skin->inverse_binds.push_back(bind_pose);
					if (godot_skeleton->get_bone_parent(bone_i) == -1) {
						gltf_skin->roots.push_back(skeleton_bone_i);
					}
					gltf_skin->joint_i_to_bone_i[bind_i] = bone_i;
					gltf_skin->joint_i_to_name[bind_i] = bind_name;
				}
				skin_gltf_i = p_state->skins.size();
				p_state->skins.push_back(gltf_skin);
				p_state->skin_and_skeleton3d_to_gltf_skin[gltf_skin_key][gltf_skel_key] = skin_gltf_i;
			}
			node->skin = skin_gltf_i;
			node->skeleton = skeleton_gltf_i;
		}
	}
}

float FBXDocument::solve_metallic(float p_dielectric_specular, float p_diffuse, float p_specular, float p_one_minus_specular_strength) {
	if (p_specular <= p_dielectric_specular) {
		return 0.0f;
	}

	const float a = p_dielectric_specular;
	const float b = p_diffuse * p_one_minus_specular_strength / (1.0f - p_dielectric_specular) + p_specular - 2.0f * p_dielectric_specular;
	const float c = p_dielectric_specular - p_specular;
	const float D = b * b - 4.0f * a * c;
	return CLAMP((-b + Math::sqrt(D)) / (2.0f * a), 0.0f, 1.0f);
}

float FBXDocument::get_perceived_brightness(const Color p_color) {
	const Color coeff = Color(R_BRIGHTNESS_COEFF, G_BRIGHTNESS_COEFF, B_BRIGHTNESS_COEFF);
	const Color value = coeff * (p_color * p_color);

	const float r = value.r;
	const float g = value.g;
	const float b = value.b;

	return Math::sqrt(r + g + b);
}

float FBXDocument::get_max_component(const Color &p_color) {
	const float r = p_color.r;
	const float g = p_color.g;
	const float b = p_color.b;

	return MAX(MAX(r, g), b);
}

void FBXDocument::_process_mesh_instances(Ref<FBXState> p_state, Node *p_scene_root) {
	for (FBXNodeIndex node_i = 0; node_i < p_state->nodes.size(); ++node_i) {
		Ref<FBXNode> node = p_state->nodes[node_i];

		if (node->skin >= 0 && node->mesh >= 0) {
			const FBXSkinIndex skin_i = node->skin;

			ImporterMeshInstance3D *mi = nullptr;
			HashMap<FBXNodeIndex, ImporterMeshInstance3D *>::Iterator mi_element = p_state->scene_mesh_instances.find(node_i);
			if (mi_element) {
				mi = mi_element->value;
			} else {
				HashMap<FBXNodeIndex, Node *>::Iterator si_element = p_state->scene_nodes.find(node_i);
				ERR_CONTINUE_MSG(!si_element, vformat("Unable to find node %d", node_i));
				mi = Object::cast_to<ImporterMeshInstance3D>(si_element->value);
				ERR_CONTINUE_MSG(mi == nullptr, vformat("Unable to cast node %d of type %s to ImporterMeshInstance3D", node_i, si_element->value->get_class_name()));
			}

			const FBXSkeletonIndex skel_i = p_state->skins.write[node->skin]->skeleton;
			Ref<FBXSkeleton> gltf_skeleton = p_state->skeletons.write[skel_i];
			Skeleton3D *skeleton = gltf_skeleton->godot_skeleton;
			ERR_CONTINUE_MSG(skeleton == nullptr, vformat("Unable to find Skeleton for node %d skin %d", node_i, skin_i));

			mi->get_parent()->remove_child(mi);
			skeleton->add_child(mi, true);
			mi->set_owner(skeleton->get_owner());

			mi->set_skin(p_state->skins.write[skin_i]->godot_skin);
			mi->set_skeleton_path(mi->get_path_to(skeleton));
			mi->set_transform(Transform3D());
		}
	}
}

FBXAnimation::Track FBXDocument::_convert_animation_track(Ref<FBXState> p_state, FBXAnimation::Track p_track, Ref<Animation> p_animation, int32_t p_track_i, FBXNodeIndex p_node_i) {
	Animation::InterpolationType interpolation = p_animation->track_get_interpolation_type(p_track_i);

	FBXAnimation::Interpolation gltf_interpolation = FBXAnimation::INTERP_LINEAR;
	if (interpolation == Animation::InterpolationType::INTERPOLATION_LINEAR) {
		gltf_interpolation = FBXAnimation::INTERP_LINEAR;
	} else if (interpolation == Animation::InterpolationType::INTERPOLATION_NEAREST) {
		gltf_interpolation = FBXAnimation::INTERP_STEP;
	} else if (interpolation == Animation::InterpolationType::INTERPOLATION_CUBIC) {
		gltf_interpolation = FBXAnimation::INTERP_CUBIC_SPLINE;
	}
	Animation::TrackType track_type = p_animation->track_get_type(p_track_i);
	int32_t key_count = p_animation->track_get_key_count(p_track_i);
	Vector<real_t> times;
	times.resize(key_count);
	String path = p_animation->track_get_path(p_track_i);
	for (int32_t key_i = 0; key_i < key_count; key_i++) {
		times.write[key_i] = p_animation->track_get_key_time(p_track_i, key_i);
	}
	double anim_end = p_animation->get_length();
	if (track_type == Animation::TYPE_SCALE_3D) {
		if (gltf_interpolation == FBXAnimation::INTERP_CUBIC_SPLINE) {
			gltf_interpolation = FBXAnimation::INTERP_LINEAR;
			p_track.scale_track.times.clear();
			p_track.scale_track.values.clear();
			// CATMULLROMSPLINE or CUBIC_SPLINE have to be baked, apologies.
			const double increment = 1.0 / BAKE_FPS;
			double time = 0.0;
			bool last = false;
			while (true) {
				Vector3 scale;
				Error err = p_animation->try_scale_track_interpolate(p_track_i, time, &scale);
				ERR_CONTINUE(err != OK);
				p_track.scale_track.values.push_back(scale);
				p_track.scale_track.times.push_back(time);
				if (last) {
					break;
				}
				time += increment;
				if (time >= anim_end) {
					last = true;
					time = anim_end;
				}
			}
		} else {
			p_track.scale_track.times = times;
			p_track.scale_track.interpolation = gltf_interpolation;
			p_track.scale_track.values.resize(key_count);
			for (int32_t key_i = 0; key_i < key_count; key_i++) {
				Vector3 scale;
				Error err = p_animation->scale_track_get_key(p_track_i, key_i, &scale);
				ERR_CONTINUE(err != OK);
				p_track.scale_track.values.write[key_i] = scale;
			}
		}
	} else if (track_type == Animation::TYPE_POSITION_3D) {
		if (gltf_interpolation == FBXAnimation::INTERP_CUBIC_SPLINE) {
			gltf_interpolation = FBXAnimation::INTERP_LINEAR;
			p_track.position_track.times.clear();
			p_track.position_track.values.clear();
			// CATMULLROMSPLINE or CUBIC_SPLINE have to be baked, apologies.
			const double increment = 1.0 / BAKE_FPS;
			double time = 0.0;
			bool last = false;
			while (true) {
				Vector3 scale;
				Error err = p_animation->try_position_track_interpolate(p_track_i, time, &scale);
				ERR_CONTINUE(err != OK);
				p_track.position_track.values.push_back(scale);
				p_track.position_track.times.push_back(time);
				if (last) {
					break;
				}
				time += increment;
				if (time >= anim_end) {
					last = true;
					time = anim_end;
				}
			}
		} else {
			p_track.position_track.times = times;
			p_track.position_track.values.resize(key_count);
			p_track.position_track.interpolation = gltf_interpolation;
			for (int32_t key_i = 0; key_i < key_count; key_i++) {
				Vector3 position;
				Error err = p_animation->position_track_get_key(p_track_i, key_i, &position);
				ERR_CONTINUE(err != OK);
				p_track.position_track.values.write[key_i] = position;
			}
		}
	} else if (track_type == Animation::TYPE_ROTATION_3D) {
		if (gltf_interpolation == FBXAnimation::INTERP_CUBIC_SPLINE) {
			gltf_interpolation = FBXAnimation::INTERP_LINEAR;
			p_track.rotation_track.times.clear();
			p_track.rotation_track.values.clear();
			// CATMULLROMSPLINE or CUBIC_SPLINE have to be baked, apologies.
			const double increment = 1.0 / BAKE_FPS;
			double time = 0.0;
			bool last = false;
			while (true) {
				Quaternion rotation;
				Error err = p_animation->try_rotation_track_interpolate(p_track_i, time, &rotation);
				ERR_CONTINUE(err != OK);
				p_track.rotation_track.values.push_back(rotation);
				p_track.rotation_track.times.push_back(time);
				if (last) {
					break;
				}
				time += increment;
				if (time >= anim_end) {
					last = true;
					time = anim_end;
				}
			}
		} else {
			p_track.rotation_track.times = times;
			p_track.rotation_track.values.resize(key_count);
			p_track.rotation_track.interpolation = gltf_interpolation;
			for (int32_t key_i = 0; key_i < key_count; key_i++) {
				Quaternion rotation;
				Error err = p_animation->rotation_track_get_key(p_track_i, key_i, &rotation);
				ERR_CONTINUE(err != OK);
				p_track.rotation_track.values.write[key_i] = rotation;
			}
		}
	} else if (track_type == Animation::TYPE_VALUE) {
		if (path.contains(":position")) {
			p_track.position_track.interpolation = gltf_interpolation;
			p_track.position_track.times = times;
			p_track.position_track.values.resize(key_count);

			if (gltf_interpolation == FBXAnimation::INTERP_CUBIC_SPLINE) {
				gltf_interpolation = FBXAnimation::INTERP_LINEAR;
				p_track.position_track.times.clear();
				p_track.position_track.values.clear();
				// CATMULLROMSPLINE or CUBIC_SPLINE have to be baked, apologies.
				const double increment = 1.0 / BAKE_FPS;
				double time = 0.0;
				bool last = false;
				while (true) {
					Vector3 position;
					Error err = p_animation->try_position_track_interpolate(p_track_i, time, &position);
					ERR_CONTINUE(err != OK);
					p_track.position_track.values.push_back(position);
					p_track.position_track.times.push_back(time);
					if (last) {
						break;
					}
					time += increment;
					if (time >= anim_end) {
						last = true;
						time = anim_end;
					}
				}
			} else {
				for (int32_t key_i = 0; key_i < key_count; key_i++) {
					Vector3 position = p_animation->track_get_key_value(p_track_i, key_i);
					p_track.position_track.values.write[key_i] = position;
				}
			}
		} else if (path.contains(":rotation")) {
			p_track.rotation_track.interpolation = gltf_interpolation;
			p_track.rotation_track.times = times;
			p_track.rotation_track.values.resize(key_count);
			if (gltf_interpolation == FBXAnimation::INTERP_CUBIC_SPLINE) {
				gltf_interpolation = FBXAnimation::INTERP_LINEAR;
				p_track.rotation_track.times.clear();
				p_track.rotation_track.values.clear();
				// CATMULLROMSPLINE or CUBIC_SPLINE have to be baked, apologies.
				const double increment = 1.0 / BAKE_FPS;
				double time = 0.0;
				bool last = false;
				while (true) {
					Quaternion rotation;
					Error err = p_animation->try_rotation_track_interpolate(p_track_i, time, &rotation);
					ERR_CONTINUE(err != OK);
					p_track.rotation_track.values.push_back(rotation);
					p_track.rotation_track.times.push_back(time);
					if (last) {
						break;
					}
					time += increment;
					if (time >= anim_end) {
						last = true;
						time = anim_end;
					}
				}
			} else {
				for (int32_t key_i = 0; key_i < key_count; key_i++) {
					Vector3 rotation_radian = p_animation->track_get_key_value(p_track_i, key_i);
					p_track.rotation_track.values.write[key_i] = Quaternion::from_euler(rotation_radian);
				}
			}
		} else if (path.contains(":scale")) {
			p_track.scale_track.times = times;
			p_track.scale_track.interpolation = gltf_interpolation;

			p_track.scale_track.values.resize(key_count);
			p_track.scale_track.interpolation = gltf_interpolation;

			if (gltf_interpolation == FBXAnimation::INTERP_CUBIC_SPLINE) {
				gltf_interpolation = FBXAnimation::INTERP_LINEAR;
				p_track.scale_track.times.clear();
				p_track.scale_track.values.clear();
				// CATMULLROMSPLINE or CUBIC_SPLINE have to be baked, apologies.
				const double increment = 1.0 / BAKE_FPS;
				double time = 0.0;
				bool last = false;
				while (true) {
					Vector3 scale;
					Error err = p_animation->try_scale_track_interpolate(p_track_i, time, &scale);
					ERR_CONTINUE(err != OK);
					p_track.scale_track.values.push_back(scale);
					p_track.scale_track.times.push_back(time);
					if (last) {
						break;
					}
					time += increment;
					if (time >= anim_end) {
						last = true;
						time = anim_end;
					}
				}
			} else {
				for (int32_t key_i = 0; key_i < key_count; key_i++) {
					Vector3 scale_track = p_animation->track_get_key_value(p_track_i, key_i);
					p_track.scale_track.values.write[key_i] = scale_track;
				}
			}
		}
	} else if (track_type == Animation::TYPE_BEZIER) {
		const int32_t keys = anim_end * BAKE_FPS;
		if (path.contains(":scale")) {
			if (!p_track.scale_track.times.size()) {
				p_track.scale_track.interpolation = gltf_interpolation;
				Vector<real_t> new_times;
				new_times.resize(keys);
				for (int32_t key_i = 0; key_i < keys; key_i++) {
					new_times.write[key_i] = key_i / BAKE_FPS;
				}
				p_track.scale_track.times = new_times;

				p_track.scale_track.values.resize(keys);

				for (int32_t key_i = 0; key_i < keys; key_i++) {
					p_track.scale_track.values.write[key_i] = Vector3(1.0f, 1.0f, 1.0f);
				}

				for (int32_t key_i = 0; key_i < keys; key_i++) {
					Vector3 bezier_track = p_track.scale_track.values[key_i];
					if (path.contains(":scale:x")) {
						bezier_track.x = p_animation->bezier_track_interpolate(p_track_i, key_i / BAKE_FPS);
					} else if (path.contains(":scale:y")) {
						bezier_track.y = p_animation->bezier_track_interpolate(p_track_i, key_i / BAKE_FPS);
					} else if (path.contains(":scale:z")) {
						bezier_track.z = p_animation->bezier_track_interpolate(p_track_i, key_i / BAKE_FPS);
					}
					p_track.scale_track.values.write[key_i] = bezier_track;
				}
			}
		} else if (path.contains(":position")) {
			if (!p_track.position_track.times.size()) {
				p_track.position_track.interpolation = gltf_interpolation;
				Vector<real_t> new_times;
				new_times.resize(keys);
				for (int32_t key_i = 0; key_i < keys; key_i++) {
					new_times.write[key_i] = key_i / BAKE_FPS;
				}
				p_track.position_track.times = new_times;

				p_track.position_track.values.resize(keys);
			}

			for (int32_t key_i = 0; key_i < keys; key_i++) {
				Vector3 bezier_track = p_track.position_track.values[key_i];
				if (path.contains(":position:x")) {
					bezier_track.x = p_animation->bezier_track_interpolate(p_track_i, key_i / BAKE_FPS);
				} else if (path.contains(":position:y")) {
					bezier_track.y = p_animation->bezier_track_interpolate(p_track_i, key_i / BAKE_FPS);
				} else if (path.contains(":position:z")) {
					bezier_track.z = p_animation->bezier_track_interpolate(p_track_i, key_i / BAKE_FPS);
				}
				p_track.position_track.values.write[key_i] = bezier_track;
			}
		} else if (path.contains(":rotation")) {
			if (!p_track.rotation_track.times.size()) {
				p_track.rotation_track.interpolation = gltf_interpolation;
				Vector<real_t> new_times;
				new_times.resize(keys);
				for (int32_t key_i = 0; key_i < keys; key_i++) {
					new_times.write[key_i] = key_i / BAKE_FPS;
				}
				p_track.rotation_track.times = new_times;

				p_track.rotation_track.values.resize(keys);
			}
			for (int32_t key_i = 0; key_i < keys; key_i++) {
				Quaternion bezier_track = p_track.rotation_track.values[key_i];
				if (path.contains(":rotation:x")) {
					bezier_track.x = p_animation->bezier_track_interpolate(p_track_i, key_i / BAKE_FPS);
				} else if (path.contains(":rotation:y")) {
					bezier_track.y = p_animation->bezier_track_interpolate(p_track_i, key_i / BAKE_FPS);
				} else if (path.contains(":rotation:z")) {
					bezier_track.z = p_animation->bezier_track_interpolate(p_track_i, key_i / BAKE_FPS);
				} else if (path.contains(":rotation:w")) {
					bezier_track.w = p_animation->bezier_track_interpolate(p_track_i, key_i / BAKE_FPS);
				}
				p_track.rotation_track.values.write[key_i] = bezier_track;
			}
		}
	}
	return p_track;
}

void FBXDocument::_convert_animation(Ref<FBXState> p_state, AnimationPlayer *p_animation_player, String p_animation_track_name) {
	Ref<Animation> animation = p_animation_player->get_animation(p_animation_track_name);
	Ref<FBXAnimation> gltf_animation;
	gltf_animation.instantiate();
	gltf_animation->set_name(_gen_unique_name(p_state, p_animation_track_name));
	for (int32_t track_i = 0; track_i < animation->get_track_count(); track_i++) {
		if (!animation->track_is_enabled(track_i)) {
			continue;
		}
		String final_track_path = animation->track_get_path(track_i);
		Node *animation_base_node = p_animation_player->get_parent();
		ERR_CONTINUE_MSG(!animation_base_node, "Cannot get the parent of the animation player.");
		if (String(final_track_path).contains(":position")) {
			const Vector<String> node_suffix = String(final_track_path).split(":position");
			const NodePath path = node_suffix[0];
			const Node *node = animation_base_node->get_node_or_null(path);
			ERR_CONTINUE_MSG(!node, "Cannot get the node from a position path.");
			for (const KeyValue<FBXNodeIndex, Node *> &position_scene_node_i : p_state->scene_nodes) {
				if (position_scene_node_i.value == node) {
					FBXNodeIndex node_index = position_scene_node_i.key;
					HashMap<int, FBXAnimation::Track>::Iterator position_track_i = gltf_animation->get_tracks().find(node_index);
					FBXAnimation::Track track;
					if (position_track_i) {
						track = position_track_i->value;
					}
					track = _convert_animation_track(p_state, track, animation, track_i, node_index);
					gltf_animation->get_tracks().insert(node_index, track);
				}
			}
		} else if (String(final_track_path).contains(":rotation_degrees")) {
			const Vector<String> node_suffix = String(final_track_path).split(":rotation_degrees");
			const NodePath path = node_suffix[0];
			const Node *node = animation_base_node->get_node_or_null(path);
			ERR_CONTINUE_MSG(!node, "Cannot get the node from a rotation degrees path.");
			for (const KeyValue<FBXNodeIndex, Node *> &rotation_degree_scene_node_i : p_state->scene_nodes) {
				if (rotation_degree_scene_node_i.value == node) {
					FBXNodeIndex node_index = rotation_degree_scene_node_i.key;
					HashMap<int, FBXAnimation::Track>::Iterator rotation_degree_track_i = gltf_animation->get_tracks().find(node_index);
					FBXAnimation::Track track;
					if (rotation_degree_track_i) {
						track = rotation_degree_track_i->value;
					}
					track = _convert_animation_track(p_state, track, animation, track_i, node_index);
					gltf_animation->get_tracks().insert(node_index, track);
				}
			}
		} else if (String(final_track_path).contains(":scale")) {
			const Vector<String> node_suffix = String(final_track_path).split(":scale");
			const NodePath path = node_suffix[0];
			const Node *node = animation_base_node->get_node_or_null(path);
			ERR_CONTINUE_MSG(!node, "Cannot get the node from a scale path.");
			for (const KeyValue<FBXNodeIndex, Node *> &scale_scene_node_i : p_state->scene_nodes) {
				if (scale_scene_node_i.value == node) {
					FBXNodeIndex node_index = scale_scene_node_i.key;
					HashMap<int, FBXAnimation::Track>::Iterator scale_track_i = gltf_animation->get_tracks().find(node_index);
					FBXAnimation::Track track;
					if (scale_track_i) {
						track = scale_track_i->value;
					}
					track = _convert_animation_track(p_state, track, animation, track_i, node_index);
					gltf_animation->get_tracks().insert(node_index, track);
				}
			}
		} else if (String(final_track_path).contains(":transform")) {
			const Vector<String> node_suffix = String(final_track_path).split(":transform");
			const NodePath path = node_suffix[0];
			const Node *node = animation_base_node->get_node_or_null(path);
			ERR_CONTINUE_MSG(!node, "Cannot get the node from a transform path.");
			for (const KeyValue<FBXNodeIndex, Node *> &transform_track_i : p_state->scene_nodes) {
				if (transform_track_i.value == node) {
					FBXAnimation::Track track;
					track = _convert_animation_track(p_state, track, animation, track_i, transform_track_i.key);
					gltf_animation->get_tracks().insert(transform_track_i.key, track);
				}
			}
		} else if (String(final_track_path).contains(":") && animation->track_get_type(track_i) == Animation::TYPE_BLEND_SHAPE) {
			const Vector<String> node_suffix = String(final_track_path).split(":");
			const NodePath path = node_suffix[0];
			const String suffix = node_suffix[1];
			Node *node = animation_base_node->get_node_or_null(path);
			ERR_CONTINUE_MSG(!node, "Cannot get the node from a blend shape path.");
			MeshInstance3D *mi = cast_to<MeshInstance3D>(node);
			if (!mi) {
				continue;
			}
			Ref<Mesh> mesh = mi->get_mesh();
			ERR_CONTINUE(mesh.is_null());
			int32_t mesh_index = -1;
			for (const KeyValue<FBXNodeIndex, Node *> &mesh_track_i : p_state->scene_nodes) {
				if (mesh_track_i.value == node) {
					mesh_index = mesh_track_i.key;
				}
			}
			ERR_CONTINUE(mesh_index == -1);
			HashMap<int, FBXAnimation::Track> &tracks = gltf_animation->get_tracks();
			FBXAnimation::Track track = gltf_animation->get_tracks().has(mesh_index) ? gltf_animation->get_tracks()[mesh_index] : FBXAnimation::Track();
			if (!tracks.has(mesh_index)) {
				for (int32_t shape_i = 0; shape_i < mesh->get_blend_shape_count(); shape_i++) {
					String shape_name = mesh->get_blend_shape_name(shape_i);
					NodePath shape_path = String(path) + ":" + shape_name;
					int32_t shape_track_i = animation->find_track(shape_path, Animation::TYPE_BLEND_SHAPE);
					if (shape_track_i == -1) {
						FBXAnimation::Channel<real_t> weight;
						weight.interpolation = FBXAnimation::INTERP_LINEAR;
						weight.times.push_back(0.0f);
						weight.times.push_back(0.0f);
						weight.values.push_back(0.0f);
						weight.values.push_back(0.0f);
						track.weight_tracks.push_back(weight);
						continue;
					}
					Animation::InterpolationType interpolation = animation->track_get_interpolation_type(track_i);
					FBXAnimation::Interpolation gltf_interpolation = FBXAnimation::INTERP_LINEAR;
					if (interpolation == Animation::InterpolationType::INTERPOLATION_LINEAR) {
						gltf_interpolation = FBXAnimation::INTERP_LINEAR;
					} else if (interpolation == Animation::InterpolationType::INTERPOLATION_NEAREST) {
						gltf_interpolation = FBXAnimation::INTERP_STEP;
					} else if (interpolation == Animation::InterpolationType::INTERPOLATION_CUBIC) {
						gltf_interpolation = FBXAnimation::INTERP_CUBIC_SPLINE;
					}
					int32_t key_count = animation->track_get_key_count(shape_track_i);
					FBXAnimation::Channel<real_t> weight;
					weight.interpolation = gltf_interpolation;
					weight.times.resize(key_count);
					for (int32_t time_i = 0; time_i < key_count; time_i++) {
						weight.times.write[time_i] = animation->track_get_key_time(shape_track_i, time_i);
					}
					weight.values.resize(key_count);
					for (int32_t value_i = 0; value_i < key_count; value_i++) {
						weight.values.write[value_i] = animation->track_get_key_value(shape_track_i, value_i);
					}
					track.weight_tracks.push_back(weight);
				}
				tracks[mesh_index] = track;
			}
		} else if (String(final_track_path).contains(":")) {
			//Process skeleton
			const Vector<String> node_suffix = String(final_track_path).split(":");
			const String node = node_suffix[0];
			const NodePath node_path = node;
			const String suffix = node_suffix[1];
			Node *godot_node = animation_base_node->get_node_or_null(node_path);
			if (!godot_node) {
				continue;
			}
			Skeleton3D *skeleton = cast_to<Skeleton3D>(animation_base_node->get_node_or_null(node));
			if (!skeleton) {
				continue;
			}
			FBXSkeletonIndex skeleton_gltf_i = -1;
			for (FBXSkeletonIndex skeleton_i = 0; skeleton_i < p_state->skeletons.size(); skeleton_i++) {
				if (p_state->skeletons[skeleton_i]->godot_skeleton == cast_to<Skeleton3D>(godot_node)) {
					skeleton = p_state->skeletons[skeleton_i]->godot_skeleton;
					skeleton_gltf_i = skeleton_i;
					ERR_CONTINUE(!skeleton);
					Ref<FBXSkeleton> skeleton_gltf = p_state->skeletons[skeleton_gltf_i];
					int32_t bone = skeleton->find_bone(suffix);
					ERR_CONTINUE_MSG(bone == -1, vformat("Cannot find the bone %s.", suffix));
					if (!skeleton_gltf->godot_bone_node.has(bone)) {
						continue;
					}
					FBXNodeIndex node_i = skeleton_gltf->godot_bone_node[bone];
					HashMap<int, FBXAnimation::Track>::Iterator property_track_i = gltf_animation->get_tracks().find(node_i);
					FBXAnimation::Track track;
					if (property_track_i) {
						track = property_track_i->value;
					}
					track = _convert_animation_track(p_state, track, animation, track_i, node_i);
					gltf_animation->get_tracks()[node_i] = track;
				}
			}
		} else if (!String(final_track_path).contains(":")) {
			ERR_CONTINUE(!animation_base_node);
			Node *godot_node = animation_base_node->get_node_or_null(final_track_path);
			ERR_CONTINUE_MSG(!godot_node, vformat("Cannot get the node from a skeleton path %s.", final_track_path));
			for (const KeyValue<FBXNodeIndex, Node *> &scene_node_i : p_state->scene_nodes) {
				if (scene_node_i.value == godot_node) {
					FBXNodeIndex node_i = scene_node_i.key;
					HashMap<int, FBXAnimation::Track>::Iterator node_track_i = gltf_animation->get_tracks().find(node_i);
					FBXAnimation::Track track;
					if (node_track_i) {
						track = node_track_i->value;
					}
					track = _convert_animation_track(p_state, track, animation, track_i, node_i);
					gltf_animation->get_tracks()[node_i] = track;
					break;
				}
			}
		}
	}
	if (gltf_animation->get_tracks().size()) {
		p_state->animations.push_back(gltf_animation);
	}
}

Error FBXDocument::_parse(Ref<FBXState> p_state, String p_path, Ref<FileAccess> p_file) {
	Error err = ERR_INVALID_DATA;
	if (p_file.is_null()) {
		return FAILED;
	}

	ufbx_load_opts opts = {};
	ufbx_error error;
	ufbx_scene *scene = ufbx_load_file((const char *)p_path.to_utf8_buffer().ptr(), &opts, &error);
	if (!scene) {
		ERR_PRINT(vformat("Failed to load: %s", error.description.data));
		return FAILED;
	}

	for (size_t i = 0; i < scene->nodes.count; i++) {
		ufbx_node *node = scene->nodes.data[i];
		if (node->is_root) {
			continue;
		}

		print_line(vformat("Object: %s", node->name.data));
		if (node->mesh) {
			print_line(vformat("-> mesh with %s faces", itos(node->mesh->faces.count)));
		}
	}	
	ERR_FAIL_NULL_V(scene, err);
	ufbx_free_scene(scene);
	document_extensions.clear();
	for (Ref<FBXDocumentExtension> ext : all_document_extensions) {
		ERR_CONTINUE(ext.is_null());
		err = ext->import_preflight(p_state, p_state->json["extensionsUsed"]);
		if (err == OK) {
			document_extensions.push_back(ext);
		}
	}

	err = _parse_fbx_state(p_state, p_path);
	ERR_FAIL_COND_V(err != OK, err);

	return OK;
}

void FBXDocument::_bind_methods() {
	ClassDB::bind_method(D_METHOD("append_from_file", "path", "state", "flags", "base_path"),
			&FBXDocument::append_from_file, DEFVAL(0), DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("append_from_buffer", "bytes", "base_path", "state", "flags"),
			&FBXDocument::append_from_buffer, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("append_from_scene", "node", "state", "flags"),
			&FBXDocument::append_from_scene, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("generate_scene", "state", "bake_fps", "trimming", "remove_immutable_tracks"),
			&FBXDocument::generate_scene, DEFVAL(30), DEFVAL(false), DEFVAL(true));
	ClassDB::bind_static_method("FBXDocument", D_METHOD("register_fbx_document_extension", "extension", "first_priority"),
			&FBXDocument::register_fbx_document_extension, DEFVAL(false));
	ClassDB::bind_static_method("FBXDocument", D_METHOD("unregister_fbx_document_extension", "extension"),
			&FBXDocument::unregister_fbx_document_extension);
}

void FBXDocument::_build_parent_hierachy(Ref<FBXState> p_state) {
	// build the hierarchy
	for (FBXNodeIndex node_i = 0; node_i < p_state->nodes.size(); node_i++) {
		for (int j = 0; j < p_state->nodes[node_i]->children.size(); j++) {
			FBXNodeIndex child_i = p_state->nodes[node_i]->children[j];
			ERR_FAIL_INDEX(child_i, p_state->nodes.size());
			if (p_state->nodes.write[child_i]->parent != -1) {
				continue;
			}
			p_state->nodes.write[child_i]->parent = node_i;
		}
	}
}

Vector<Ref<FBXDocumentExtension>> FBXDocument::all_document_extensions;

void FBXDocument::register_fbx_document_extension(Ref<FBXDocumentExtension> p_extension, bool p_first_priority) {
	if (all_document_extensions.find(p_extension) == -1) {
		if (p_first_priority) {
			all_document_extensions.insert(0, p_extension);
		} else {
			all_document_extensions.push_back(p_extension);
		}
	}
}

void FBXDocument::unregister_fbx_document_extension(Ref<FBXDocumentExtension> p_extension) {
	all_document_extensions.erase(p_extension);
}

void FBXDocument::unregister_all_fbx_document_extensions() {
	all_document_extensions.clear();
}

Node *FBXDocument::generate_scene(Ref<FBXState> p_state, float p_bake_fps, bool p_trimming, bool p_remove_immutable_tracks) {
	ERR_FAIL_NULL_V(p_state, nullptr);
	ERR_FAIL_INDEX_V(0, p_state->root_nodes.size(), nullptr);
	Error err = OK;
	FBXNodeIndex gltf_root = p_state->root_nodes.write[0];
	Node *gltf_root_node = p_state->get_scene_node(gltf_root);
	Node *root = gltf_root_node->get_parent();
	ERR_FAIL_NULL_V(root, nullptr);
	_process_mesh_instances(p_state, root);
	if (p_state->get_create_animations() && p_state->animations.size()) {
		AnimationPlayer *ap = memnew(AnimationPlayer);
		root->add_child(ap, true);
		ap->set_owner(root);
		for (int i = 0; i < p_state->animations.size(); i++) {
			_import_animation(p_state, ap, i, p_bake_fps, p_trimming, p_remove_immutable_tracks);
		}
	}
	for (KeyValue<FBXNodeIndex, Node *> E : p_state->scene_nodes) {
		ERR_CONTINUE(!E.value);
		for (Ref<FBXDocumentExtension> ext : document_extensions) {
			ERR_CONTINUE(ext.is_null());
			ERR_CONTINUE(!p_state->json.has("nodes"));
			Array nodes = p_state->json["nodes"];
			ERR_CONTINUE(E.key >= nodes.size());
			ERR_CONTINUE(E.key < 0);
			Dictionary node_json = nodes[E.key];
			Ref<FBXNode> gltf_node = p_state->nodes[E.key];
			err = ext->import_node(p_state, gltf_node, node_json, E.value);
			ERR_CONTINUE(err != OK);
		}
	}
	for (Ref<FBXDocumentExtension> ext : document_extensions) {
		ERR_CONTINUE(ext.is_null());
		err = ext->import_post(p_state, root);
		ERR_CONTINUE(err != OK);
	}
	ERR_FAIL_NULL_V(root, nullptr);
	return root;
}

Error FBXDocument::append_from_scene(Node *p_node, Ref<FBXState> p_state, uint32_t p_flags) {
	ERR_FAIL_COND_V(p_state.is_null(), FAILED);
	p_state->use_named_skin_binds = p_flags & FBX_IMPORT_USE_NAMED_SKIN_BINDS;
	p_state->discard_meshes_and_materials = p_flags & FBX_IMPORT_DISCARD_MESHES_AND_MATERIALS;
	if (!p_state->buffers.size()) {
		p_state->buffers.push_back(Vector<uint8_t>());
	}
	// Perform export preflight for document extensions. Only extensions that
	// return OK will be used for the rest of the export steps.
	document_extensions.clear();
	for (Ref<FBXDocumentExtension> ext : all_document_extensions) {
		ERR_CONTINUE(ext.is_null());
		Error err = ext->export_preflight(p_state, p_node);
		if (err == OK) {
			document_extensions.push_back(ext);
		}
	}
	// Add the root node(s) and their descendants to the state.
	_convert_scene_node(p_state, p_node, -1, -1);
	return OK;
}

Error FBXDocument::append_from_buffer(PackedByteArray p_bytes, String p_base_path, Ref<FBXState> p_state, uint32_t p_flags) {
	ERR_FAIL_COND_V(p_state.is_null(), FAILED);
	// TODO Add missing texture and missing .bin file paths to r_missing_deps 2021-09-10 fire
	Error err = FAILED;
	p_state->use_named_skin_binds = p_flags & FBX_IMPORT_USE_NAMED_SKIN_BINDS;
	p_state->discard_meshes_and_materials = p_flags & FBX_IMPORT_DISCARD_MESHES_AND_MATERIALS;

	Ref<FileAccessMemory> file_access;
	file_access.instantiate();
	file_access->open_custom(p_bytes.ptr(), p_bytes.size());
	p_state->base_path = p_base_path.get_base_dir();
	err = _parse(p_state, p_state->base_path, file_access);
	ERR_FAIL_COND_V(err != OK, err);
	for (Ref<FBXDocumentExtension> ext : document_extensions) {
		ERR_CONTINUE(ext.is_null());
		err = ext->import_post_parse(p_state);
		ERR_FAIL_COND_V(err != OK, err);
	}
	return OK;
}

Error FBXDocument::_parse_fbx_state(Ref<FBXState> p_state, const String &p_search_path) {
	Error err;

	/* PARSE EXTENSIONS */
	err = _parse_fbx_extensions(p_state);
	ERR_FAIL_COND_V(err != OK, ERR_PARSE_ERROR);

	/* PARSE SCENE */
	err = _parse_scenes(p_state);
	ERR_FAIL_COND_V(err != OK, ERR_PARSE_ERROR);

	/* PARSE NODES */
	err = _parse_nodes(p_state);
	ERR_FAIL_COND_V(err != OK, ERR_PARSE_ERROR);

	/* PARSE BUFFERS */
	err = _parse_buffers(p_state, p_search_path);

	ERR_FAIL_COND_V(err != OK, ERR_PARSE_ERROR);

	/* PARSE BUFFER VIEWS */
	err = _parse_buffer_views(p_state);

	ERR_FAIL_COND_V(err != OK, ERR_PARSE_ERROR);

	/* PARSE ACCESSORS */
	err = _parse_accessors(p_state);

	ERR_FAIL_COND_V(err != OK, ERR_PARSE_ERROR);

	if (!p_state->discard_meshes_and_materials) {
		/* PARSE IMAGES */
		err = _parse_images(p_state, p_search_path);

		ERR_FAIL_COND_V(err != OK, ERR_PARSE_ERROR);

		/* PARSE TEXTURE SAMPLERS */
		err = _parse_texture_samplers(p_state);

		ERR_FAIL_COND_V(err != OK, ERR_PARSE_ERROR);

		/* PARSE TEXTURES */
		err = _parse_textures(p_state);

		ERR_FAIL_COND_V(err != OK, ERR_PARSE_ERROR);

		/* PARSE TEXTURES */
		err = _parse_materials(p_state);

		ERR_FAIL_COND_V(err != OK, ERR_PARSE_ERROR);
	}

	/* PARSE SKINS */
	err = _parse_skins(p_state);

	ERR_FAIL_COND_V(err != OK, ERR_PARSE_ERROR);

	/* DETERMINE SKELETONS */
	err = _determine_skeletons(p_state);
	ERR_FAIL_COND_V(err != OK, ERR_PARSE_ERROR);

	/* CREATE SKELETONS */
	err = _create_skeletons(p_state);
	ERR_FAIL_COND_V(err != OK, ERR_PARSE_ERROR);

	/* CREATE SKINS */
	err = _create_skins(p_state);
	ERR_FAIL_COND_V(err != OK, ERR_PARSE_ERROR);

	/* PARSE MESHES (we have enough info now) */
	err = _parse_meshes(p_state);
	ERR_FAIL_COND_V(err != OK, ERR_PARSE_ERROR);

	/* PARSE CAMERAS */
	err = _parse_cameras(p_state);
	ERR_FAIL_COND_V(err != OK, ERR_PARSE_ERROR);

	/* PARSE ANIMATIONS */
	err = _parse_animations(p_state);
	ERR_FAIL_COND_V(err != OK, ERR_PARSE_ERROR);

	/* ASSIGN SCENE NAMES */
	_assign_node_names(p_state);

	Node3D *root = memnew(Node3D);
	for (int32_t root_i = 0; root_i < p_state->root_nodes.size(); root_i++) {
		_generate_scene_node(p_state, p_state->root_nodes[root_i], root, root);
	}

	return OK;
}

Error FBXDocument::append_from_file(String p_path, Ref<FBXState> p_state, uint32_t p_flags, String p_base_path) {
	// TODO Add missing texture and missing .bin file paths to r_missing_deps 2021-09-10 fire
	if (p_state == Ref<FBXState>()) {
		p_state.instantiate();
	}
	p_state->filename = p_path.get_file().get_basename();
	p_state->use_named_skin_binds = p_flags & FBX_IMPORT_USE_NAMED_SKIN_BINDS;
	p_state->discard_meshes_and_materials = p_flags & FBX_IMPORT_DISCARD_MESHES_AND_MATERIALS;
	Error err;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ, &err);
	ERR_FAIL_COND_V(err != OK, ERR_FILE_CANT_OPEN);
	ERR_FAIL_NULL_V(file, ERR_FILE_CANT_OPEN);
	String base_path = p_base_path;
	if (base_path.is_empty()) {
		base_path = p_path.get_base_dir();
	}
	p_state->base_path = base_path;
	err = _parse(p_state, base_path, file);
	ERR_FAIL_COND_V(err != OK, err);
	for (Ref<FBXDocumentExtension> ext : document_extensions) {
		ERR_CONTINUE(ext.is_null());
		err = ext->import_post_parse(p_state);
		ERR_FAIL_COND_V(err != OK, err);
	}
	return OK;
}

Error FBXDocument::_parse_fbx_extensions(Ref<FBXState> p_state) {
	ERR_FAIL_NULL_V(p_state, ERR_PARSE_ERROR);
	if (p_state->json.has("extensionsUsed")) {
		Vector<String> ext_array = p_state->json["extensionsUsed"];
		p_state->extensions_used = ext_array;
	}
	if (p_state->json.has("extensionsRequired")) {
		Vector<String> ext_array = p_state->json["extensionsRequired"];
		p_state->extensions_required = ext_array;
	}
	HashSet<String> supported_extensions;
	supported_extensions.insert("KHR_lights_punctual");
	supported_extensions.insert("KHR_materials_pbrSpecularGlossiness");
	supported_extensions.insert("KHR_texture_transform");
	supported_extensions.insert("KHR_materials_unlit");
	supported_extensions.insert("KHR_materials_emissive_strength");
	for (Ref<FBXDocumentExtension> ext : document_extensions) {
		ERR_CONTINUE(ext.is_null());
		Vector<String> ext_supported_extensions = ext->get_supported_extensions();
		for (int i = 0; i < ext_supported_extensions.size(); ++i) {
			supported_extensions.insert(ext_supported_extensions[i]);
		}
	}
	Error ret = OK;
	for (int i = 0; i < p_state->extensions_required.size(); i++) {
		if (!supported_extensions.has(p_state->extensions_required[i])) {
			ERR_PRINT("GLTF: Can't import file '" + p_state->filename + "', required extension '" + String(p_state->extensions_required[i]) + "' is not supported. Are you missing a FBXDocumentExtension plugin?");
			ret = ERR_UNAVAILABLE;
		}
	}
	return ret;
}