/*******************************************************************************
 *
 * Module Name: dsmthdat - control method arguments and local variables
 *
 ******************************************************************************/

/*
 * Copyright (C) 2000 - 2003, R. Byron Moore
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce at minimum a disclaimer
 *    substantially similar to the "NO WARRANTY" disclaimer below
 *    ("Disclaimer") and any redistribution must be conditioned upon
 *    including a substantially similar Disclaimer requirement for further
 *    binary redistribution.
 * 3. Neither the names of the above-listed copyright holders nor the names
 *    of any contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * NO WARRANTY
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGES.
 */


#include <acpi/acpi.h>
#include <acpi/acdispat.h>
#include <acpi/amlcode.h>
#include <acpi/acnamesp.h>
#include <acpi/acinterp.h>


#define _COMPONENT          ACPI_DISPATCHER
	 ACPI_MODULE_NAME    ("dsmthdat")


/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_method_data_init
 *
 * PARAMETERS:  walk_state          - Current walk state object
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Initialize the data structures that hold the method's arguments
 *              and locals.  The data struct is an array of NTEs for each.
 *              This allows ref_of and de_ref_of to work properly for these
 *              special data types.
 *
 * NOTES:       walk_state fields are initialized to zero by the
 *              ACPI_MEM_CALLOCATE().
 *
 *              A pseudo-Namespace Node is assigned to each argument and local
 *              so that ref_of() can return a pointer to the Node.
 *
 ******************************************************************************/

void
acpi_ds_method_data_init (
	struct acpi_walk_state          *walk_state)
{
	u32                             i;


	ACPI_FUNCTION_TRACE ("ds_method_data_init");


	/* Init the method arguments */

	for (i = 0; i < ACPI_METHOD_NUM_ARGS; i++) {
		ACPI_MOVE_UNALIGNED32_TO_32 (&walk_state->arguments[i].name,
				 NAMEOF_ARG_NTE);
		walk_state->arguments[i].name.integer |= (i << 24);
		walk_state->arguments[i].descriptor   = ACPI_DESC_TYPE_NAMED;
		walk_state->arguments[i].type         = ACPI_TYPE_ANY;
		walk_state->arguments[i].flags        = ANOBJ_END_OF_PEER_LIST | ANOBJ_METHOD_ARG;
	}

	/* Init the method locals */

	for (i = 0; i < ACPI_METHOD_NUM_LOCALS; i++) {
		ACPI_MOVE_UNALIGNED32_TO_32 (&walk_state->local_variables[i].name,
				 NAMEOF_LOCAL_NTE);

		walk_state->local_variables[i].name.integer |= (i << 24);
		walk_state->local_variables[i].descriptor  = ACPI_DESC_TYPE_NAMED;
		walk_state->local_variables[i].type        = ACPI_TYPE_ANY;
		walk_state->local_variables[i].flags       = ANOBJ_END_OF_PEER_LIST | ANOBJ_METHOD_LOCAL;
	}

	return_VOID;
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_method_data_delete_all
 *
 * PARAMETERS:  walk_state          - Current walk state object
 *
 * RETURN:      None
 *
 * DESCRIPTION: Delete method locals and arguments.  Arguments are only
 *              deleted if this method was called from another method.
 *
 ******************************************************************************/

void
acpi_ds_method_data_delete_all (
	struct acpi_walk_state          *walk_state)
{
	u32                             index;


	ACPI_FUNCTION_TRACE ("ds_method_data_delete_all");


	/* Detach the locals */

	for (index = 0; index < ACPI_METHOD_NUM_LOCALS; index++) {
		if (walk_state->local_variables[index].object) {
			ACPI_DEBUG_PRINT ((ACPI_DB_EXEC, "Deleting Local%d=%p\n",
					index, walk_state->local_variables[index].object));

			/* Detach object (if present) and remove a reference */

			acpi_ns_detach_object (&walk_state->local_variables[index]);
		}
	}

	/* Detach the arguments */

	for (index = 0; index < ACPI_METHOD_NUM_ARGS; index++) {
		if (walk_state->arguments[index].object) {
			ACPI_DEBUG_PRINT ((ACPI_DB_EXEC, "Deleting Arg%d=%p\n",
					index, walk_state->arguments[index].object));

			/* Detach object (if present) and remove a reference */

			acpi_ns_detach_object (&walk_state->arguments[index]);
		}
	}

	return_VOID;
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_method_data_init_args
 *
 * PARAMETERS:  *Params         - Pointer to a parameter list for the method
 *              max_param_count - The arg count for this method
 *              walk_state      - Current walk state object
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Initialize arguments for a method.  The parameter list is a list
 *              of ACPI operand objects, either null terminated or whose length
 *              is defined by max_param_count.
 *
 ******************************************************************************/

acpi_status
acpi_ds_method_data_init_args (
	union acpi_operand_object       **params,
	u32                             max_param_count,
	struct acpi_walk_state          *walk_state)
{
	acpi_status                     status;
	u32                             index = 0;


	ACPI_FUNCTION_TRACE_PTR ("ds_method_data_init_args", params);


	if (!params) {
		ACPI_DEBUG_PRINT ((ACPI_DB_EXEC, "No param list passed to method\n"));
		return_ACPI_STATUS (AE_OK);
	}

	/* Copy passed parameters into the new method stack frame  */

	while ((index < ACPI_METHOD_NUM_ARGS) && (index < max_param_count) && params[index]) {
		/*
		 * A valid parameter.
		 * Store the argument in the method/walk descriptor
		 */
		status = acpi_ds_store_object_to_local (AML_ARG_OP, index, params[index],
				 walk_state);
		if (ACPI_FAILURE (status)) {
			return_ACPI_STATUS (status);
		}

		index++;
	}

	ACPI_DEBUG_PRINT ((ACPI_DB_EXEC, "%d args passed to method\n", index));
	return_ACPI_STATUS (AE_OK);
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_method_data_get_node
 *
 * PARAMETERS:  Opcode              - Either AML_LOCAL_OP or AML_ARG_OP
 *              Index               - which local_var or argument whose type
 *                                      to get
 *              walk_state          - Current walk state object
 *
 * RETURN:      Get the Node associated with a local or arg.
 *
 ******************************************************************************/

acpi_status
acpi_ds_method_data_get_node (
	u16                             opcode,
	u32                             index,
	struct acpi_walk_state          *walk_state,
	struct acpi_namespace_node      **node)
{
	ACPI_FUNCTION_TRACE ("ds_method_data_get_node");


	/*
	 * Method Locals and Arguments are supported
	 */
	switch (opcode) {
	case AML_LOCAL_OP:

		if (index > ACPI_METHOD_MAX_LOCAL) {
			ACPI_DEBUG_PRINT ((ACPI_DB_ERROR, "Local index %d is invalid (max %d)\n",
				index, ACPI_METHOD_MAX_LOCAL));
			return_ACPI_STATUS (AE_AML_INVALID_INDEX);
		}

		/* Return a pointer to the pseudo-node */

		*node = &walk_state->local_variables[index];
		break;

	case AML_ARG_OP:

		if (index > ACPI_METHOD_MAX_ARG) {
			ACPI_DEBUG_PRINT ((ACPI_DB_ERROR, "Arg index %d is invalid (max %d)\n",
				index, ACPI_METHOD_MAX_ARG));
			return_ACPI_STATUS (AE_AML_INVALID_INDEX);
		}

		/* Return a pointer to the pseudo-node */

		*node = &walk_state->arguments[index];
		break;

	default:
		ACPI_DEBUG_PRINT ((ACPI_DB_ERROR, "Opcode %d is invalid\n", opcode));
		return_ACPI_STATUS (AE_AML_BAD_OPCODE);
	}

	return_ACPI_STATUS (AE_OK);
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_method_data_set_value
 *
 * PARAMETERS:  Opcode              - Either AML_LOCAL_OP or AML_ARG_OP
 *              Index               - which local_var or argument to get
 *              Object              - Object to be inserted into the stack entry
 *              walk_state          - Current walk state object
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Insert an object onto the method stack at entry Opcode:Index.
 *              Note: There is no "implicit conversion" for locals.
 *
 ******************************************************************************/

acpi_status
acpi_ds_method_data_set_value (
	u16                             opcode,
	u32                             index,
	union acpi_operand_object       *object,
	struct acpi_walk_state          *walk_state)
{
	acpi_status                     status;
	struct acpi_namespace_node      *node;
	union acpi_operand_object       *new_desc = object;


	ACPI_FUNCTION_TRACE ("ds_method_data_set_value");


	ACPI_DEBUG_PRINT ((ACPI_DB_EXEC,
		"obj %p op %X, ref count = %d [%s]\n", object,
		opcode, object->common.reference_count,
		acpi_ut_get_type_name (object->common.type)));

	/* Get the namespace node for the arg/local */

	status = acpi_ds_method_data_get_node (opcode, index, walk_state, &node);
	if (ACPI_FAILURE (status)) {
		return_ACPI_STATUS (status);
	}

	/*
	 * If the object has just been created and is not attached to anything,
	 * (the reference count is 1), then we can just store it directly into
	 * the arg/local.  Otherwise, we must copy it.
	 */
	if (object->common.reference_count > 1) {
		status = acpi_ut_copy_iobject_to_iobject (object, &new_desc, walk_state);
		if (ACPI_FAILURE (status)) {
			return_ACPI_STATUS (status);
		}

	   ACPI_DEBUG_PRINT ((ACPI_DB_EXEC, "Object Copied %p, new %p\n",
		   object, new_desc));
	}
	else {
		/* Increment ref count so object can't be deleted while installed */

		acpi_ut_add_reference (new_desc);
	}

	/* Install the object */

	node->object = new_desc;
	return_ACPI_STATUS (status);
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_method_data_get_type
 *
 * PARAMETERS:  Opcode              - Either AML_LOCAL_OP or AML_ARG_OP
 *              Index               - which local_var or argument whose type
 *                                      to get
 *              walk_state          - Current walk state object
 *
 * RETURN:      Data type of current value of the selected Arg or Local
 *
 ******************************************************************************/

acpi_object_type
acpi_ds_method_data_get_type (
	u16                             opcode,
	u32                             index,
	struct acpi_walk_state          *walk_state)
{
	acpi_status                     status;
	struct acpi_namespace_node      *node;
	union acpi_operand_object       *object;


	ACPI_FUNCTION_TRACE ("ds_method_data_get_type");


	/* Get the namespace node for the arg/local */

	status = acpi_ds_method_data_get_node (opcode, index, walk_state, &node);
	if (ACPI_FAILURE (status)) {
		return_VALUE ((ACPI_TYPE_NOT_FOUND));
	}

	/* Get the object */

	object = acpi_ns_get_attached_object (node);
	if (!object) {
		/* Uninitialized local/arg, return TYPE_ANY */

		return_VALUE (ACPI_TYPE_ANY);
	}

	/* Get the object type */

	return_VALUE (ACPI_GET_OBJECT_TYPE (object));
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_method_data_get_value
 *
 * PARAMETERS:  Opcode              - Either AML_LOCAL_OP or AML_ARG_OP
 *              Index               - which local_var or argument to get
 *              walk_state          - Current walk state object
 *              *dest_desc          - Ptr to Descriptor into which selected Arg
 *                                    or Local value should be copied
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Retrieve value of selected Arg or Local from the method frame
 *              at the current top of the method stack.
 *              Used only in acpi_ex_resolve_to_value().
 *
 ******************************************************************************/

acpi_status
acpi_ds_method_data_get_value (
	u16                             opcode,
	u32                             index,
	struct acpi_walk_state          *walk_state,
	union acpi_operand_object       **dest_desc)
{
	acpi_status                     status;
	struct acpi_namespace_node      *node;
	union acpi_operand_object       *object;


	ACPI_FUNCTION_TRACE ("ds_method_data_get_value");


	/* Validate the object descriptor */

	if (!dest_desc) {
		ACPI_DEBUG_PRINT ((ACPI_DB_ERROR, "Null object descriptor pointer\n"));
		return_ACPI_STATUS (AE_BAD_PARAMETER);
	}

	/* Get the namespace node for the arg/local */

	status = acpi_ds_method_data_get_node (opcode, index, walk_state, &node);
	if (ACPI_FAILURE (status)) {
		return_ACPI_STATUS (status);
	}

	/* Get the object from the node */

	object = node->object;

	/* Examine the returned object, it must be valid. */

	if (!object) {
		/*
		 * Index points to uninitialized object.
		 * This means that either 1) The expected argument was
		 * not passed to the method, or 2) A local variable
		 * was referenced by the method (via the ASL)
		 * before it was initialized.  Either case is an error.
		 */
		switch (opcode) {
		case AML_ARG_OP:

			ACPI_DEBUG_PRINT ((ACPI_DB_ERROR, "Uninitialized Arg[%d] at node %p\n",
				index, node));

			return_ACPI_STATUS (AE_AML_UNINITIALIZED_ARG);

		case AML_LOCAL_OP:

			ACPI_DEBUG_PRINT ((ACPI_DB_ERROR, "Uninitialized Local[%d] at node %p\n",
				index, node));

			return_ACPI_STATUS (AE_AML_UNINITIALIZED_LOCAL);

		default:
			return_ACPI_STATUS (AE_AML_INTERNAL);
		}
	}

	/*
	 * The Index points to an initialized and valid object.
	 * Return an additional reference to the object
	 */
	*dest_desc = object;
	acpi_ut_add_reference (object);

	return_ACPI_STATUS (AE_OK);
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_method_data_delete_value
 *
 * PARAMETERS:  Opcode              - Either AML_LOCAL_OP or AML_ARG_OP
 *              Index               - which local_var or argument to delete
 *              walk_state          - Current walk state object
 *
 * RETURN:      None
 *
 * DESCRIPTION: Delete the entry at Opcode:Index on the method stack.  Inserts
 *              a null into the stack slot after the object is deleted.
 *
 ******************************************************************************/

void
acpi_ds_method_data_delete_value (
	u16                             opcode,
	u32                             index,
	struct acpi_walk_state          *walk_state)
{
	acpi_status                     status;
	struct acpi_namespace_node      *node;
	union acpi_operand_object       *object;


	ACPI_FUNCTION_TRACE ("ds_method_data_delete_value");


	/* Get the namespace node for the arg/local */

	status = acpi_ds_method_data_get_node (opcode, index, walk_state, &node);
	if (ACPI_FAILURE (status)) {
		return_VOID;
	}

	/* Get the associated object */

	object = acpi_ns_get_attached_object (node);

	/*
	 * Undefine the Arg or Local by setting its descriptor
	 * pointer to NULL. Locals/Args can contain both
	 * ACPI_OPERAND_OBJECTS and ACPI_NAMESPACE_NODEs
	 */
	node->object = NULL;

	if ((object) &&
		(ACPI_GET_DESCRIPTOR_TYPE (object) == ACPI_DESC_TYPE_OPERAND)) {
		/*
		 * There is a valid object.
		 * Decrement the reference count by one to balance the
		 * increment when the object was stored.
		 */
		acpi_ut_remove_reference (object);
	}

	return_VOID;
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_store_object_to_local
 *
 * PARAMETERS:  Opcode              - Either AML_LOCAL_OP or AML_ARG_OP
 *              Index               - which local_var or argument to set
 *              obj_desc            - Value to be stored
 *              walk_state          - Current walk state
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Store a value in an Arg or Local.  The obj_desc is installed
 *              as the new value for the Arg or Local and the reference count
 *              for obj_desc is incremented.
 *
 ******************************************************************************/

acpi_status
acpi_ds_store_object_to_local (
	u16                             opcode,
	u32                             index,
	union acpi_operand_object       *obj_desc,
	struct acpi_walk_state          *walk_state)
{
	acpi_status                     status;
	struct acpi_namespace_node      *node;
	union acpi_operand_object       *current_obj_desc;


	ACPI_FUNCTION_TRACE ("ds_store_object_to_local");
	ACPI_DEBUG_PRINT ((ACPI_DB_EXEC, "Opcode=%d Idx=%d Obj=%p\n",
		opcode, index, obj_desc));


	/* Parameter validation */

	if (!obj_desc) {
		return_ACPI_STATUS (AE_BAD_PARAMETER);
	}

	/* Get the namespace node for the arg/local */

	status = acpi_ds_method_data_get_node (opcode, index, walk_state, &node);
	if (ACPI_FAILURE (status)) {
		return_ACPI_STATUS (status);
	}

	current_obj_desc = acpi_ns_get_attached_object (node);
	if (current_obj_desc == obj_desc) {
		ACPI_DEBUG_PRINT ((ACPI_DB_EXEC, "Obj=%p already installed!\n",
			obj_desc));
		return_ACPI_STATUS (status);
	}

	/*
	 * If there is an object already in this slot, we either
	 * have to delete it, or if this is an argument and there
	 * is an object reference stored there, we have to do
	 * an indirect store!
	 */
	if (current_obj_desc) {
		/*
		 * Check for an indirect store if an argument
		 * contains an object reference (stored as an Node).
		 * We don't allow this automatic dereferencing for
		 * locals, since a store to a local should overwrite
		 * anything there, including an object reference.
		 *
		 * If both Arg0 and Local0 contain ref_of (Local4):
		 *
		 * Store (1, Arg0)             - Causes indirect store to local4
		 * Store (1, Local0)           - Stores 1 in local0, overwriting
		 *                                  the reference to local4
		 * Store (1, de_refof (Local0)) - Causes indirect store to local4
		 *
		 * Weird, but true.
		 */
		if (opcode == AML_ARG_OP) {
			/*
			 * Make sure that the object is the correct type.  This may be overkill, but
			 * it is here because references were NS nodes in the past.  Now they are
			 * operand objects of type Reference.
			 */
			if (ACPI_GET_DESCRIPTOR_TYPE (current_obj_desc) != ACPI_DESC_TYPE_OPERAND) {
				ACPI_REPORT_ERROR (("Invalid descriptor type while storing to method arg: %X\n",
					current_obj_desc->common.type));
				return_ACPI_STATUS (AE_AML_INTERNAL);
			}

			/*
			 * If we have a valid reference object that came from ref_of(), do the
			 * indirect store
			 */
			if ((current_obj_desc->common.type == ACPI_TYPE_LOCAL_REFERENCE) &&
				(current_obj_desc->reference.opcode == AML_REF_OF_OP)) {
				ACPI_DEBUG_PRINT ((ACPI_DB_EXEC,
					"Arg (%p) is an obj_ref(Node), storing in node %p\n",
					obj_desc, current_obj_desc));

				/*
				 * Store this object to the Node
				 * (perform the indirect store)
				 */
				status = acpi_ex_store_object_to_node (obj_desc,
						 current_obj_desc->reference.object, walk_state);
				return_ACPI_STATUS (status);
			}
		}

		/*
		 * Delete the existing object
		 * before storing the new one
		 */
		acpi_ds_method_data_delete_value (opcode, index, walk_state);
	}

	/*
	 * Install the obj_stack descriptor (*obj_desc) into
	 * the descriptor for the Arg or Local.
	 * Install the new object in the stack entry
	 * (increments the object reference count by one)
	 */
	status = acpi_ds_method_data_set_value (opcode, index, obj_desc, walk_state);
	return_ACPI_STATUS (status);
}


