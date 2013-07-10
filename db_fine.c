#include "db.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

/* Forward declaration */
node_t *searchQ(char *, node_t *, node_t **);
node_t *searchAR(char *, node_t *, node_t **);
//node_t *searchR(char *, node_t *, node_t **);

node_t head = { "", "", 0, 0, PTHREAD_RWLOCK_INITIALIZER };
/*
 * Allocate a new node with the given key, value and children.
 */
node_t *node_create(char *arg_name, char *arg_value, node_t * arg_left, node_t * arg_right) 
{
    node_t *new_node;

    new_node = (node_t *) malloc(sizeof(node_t));

    if (!new_node)
    {
    	return NULL;
    }

    //Initialize the rwlock
    pthread_rwlock_init(&(new_node->mutex_node_lock),NULL);

    if (!(new_node->name = (char *)malloc(strlen(arg_name) + 1))) 
    {
		free(new_node);
		return NULL;
    }

    if (!(new_node->value = (char *)malloc(strlen(arg_value) + 1))) 
    {
		free(new_node->name);
		free(new_node);
		return NULL;
    }

    strcpy(new_node->name, arg_name);
    strcpy(new_node->value, arg_value);
    new_node->lchild = arg_left;
    new_node->rchild = arg_right;
    
    return new_node;
}

/* Free the data structures in node and the node itself. */
void node_destroy(node_t * node) {
    /* Clearing name and value after they are freed is defensive programming in
     * case the node_destroy is called again. */
	//Destroy the rwlock
	pthread_rwlock_destroy(&(node->mutex_node_lock));

    if (node->name) {free(node->name); node->name = NULL; }
    if (node->value) { free(node->value); node->value = NULL; }
    free(node);
}

/* Find the node with key name and return a result or error string in result.
 * Result must have space for len characters. */
void query(char *name, char *result, int len) 
{
	node_t *parent;
    node_t *target;

    //Parent will be locked afte this
    target = searchQ(name, &head, &parent);

    if (!target) 
    {
		strncpy(result, "not found", len - 1);
		//Release the parent lock
		pthread_rwlock_unlock(&(parent->mutex_node_lock));
		return;
    } 
    else 
    {   	
	    //The only critical section for the read	
		strncpy(result, target->value, len - 1);
		//Unlock any of the locks
		pthread_rwlock_unlock(&(target->mutex_node_lock));
		pthread_rwlock_unlock(&(parent->mutex_node_lock));
		return;
    }
}

/* Insert a node with name and value into the proper place in the DB rooted at
 * head. */
int add(char *name, char *value) {
	node_t *parent;	    /* The new node will be the child of this node */
	node_t *target;	    /* The existing node with key name if any */
	node_t *newnode;    /* The new node to add */

	//Target and parent will be locked after this
	if ((target = searchAR(name, &head, &parent))) 
	{
	    /* There is already a node with this key in the tree */
	    pthread_rwlock_unlock(&(target->mutex_node_lock));
	    pthread_rwlock_unlock(&(parent->mutex_node_lock));
	    return 0;
	}
	/* No idea how this could happen, but... */
	if (!parent) return 0;

	/* make the new node and attach it to parent */
	newnode = node_create(name, value, 0, 0);

	if (strcmp(name, parent->name) < 0) 
	{
		parent->lchild = newnode;
	}
	else 
	{
		parent->rchild = newnode;
	}
	//Unlock the lock
	pthread_rwlock_unlock(&(parent->mutex_node_lock));

	return 1;
}

/*
 * When deleting a node with 2 children, we swap the contents leftmost child of
 * its right subtree with the node to be deleted.  This is used to swap those
 * content pointers without copying the data, which is unsafe if the
 * allocations are different sizes (copying "alamorgodo" into "ny" for
 * example).
 */
static inline void swap_pointers(char **a, char **b) 
{
    char *tmp = *b;
    *b = *a;
    *a = tmp;
}

/* Remove the node with key name from the tree if it is there.  See inline
 * comments for algorithmic details.  Return true if something was deleted. */
int xremove(char *name) 
{
	node_t *parent;	    /* Parent of the node to delete */
	node_t *dnode;	    /* Node to delete */
	node_t *next;	    /* used to find leftmost child of right subtree */
	node_t **pnext;	    /* A pointer in the tree that points to next so we
			       can change that nodes children (see below). */

	/* first, find the node to be removed */
	if (!(dnode = searchAR(name, &head, &parent))) 
	{
		pthread_rwlock_unlock(&(parent->mutex_node_lock));
	    /* it's not there */
	    return 0;
	}

	/* we found it.  Now check out the easy cases.  If the node has no
	 * right child, then we can merely replace its parent's pointer to
	 * it with the node's left child. */
	if (dnode->rchild == 0) 
	{
		//Has only left child
		//I think this also handles no children
	    if (strcmp(dnode->name, parent->name) < 0)
	    {
	    	//It is the left child of the parent
			parent->lchild = dnode->lchild;
		}
	    else
	    {
	    	//It is the right child of the parent
			parent->rchild = dnode->lchild;
		}

	    /* done with dnode */
	    node_destroy(dnode);
	} 
	else if (dnode->lchild == 0) 
	{
		//Has only right child
    	/* ditto if the node had no left child */
    	if (strcmp(dnode->name, parent->name) < 0)
    	{
  			//IS the left child of the parent
			parent->lchild = dnode->rchild;
		}
    	else
    	{
			parent->rchild = dnode->rchild;
		}

    	/* done with dnode */
    	node_destroy(dnode);
	} 
	else 
	{
	    /* So much for the easy cases ...
	     * We know that all nodes in a node's right subtree have
	     * lexicographically greater names than the node does, and all
	     * nodes in a node's left subtree have lexicographically smaller
	     * names than the node does. So, we find the lexicographically
	     * smallest node in the right subtree and replace the node to be
	     * deleted with that node. This new node thus is lexicographically
	     * smaller than all nodes in its right subtree, and greater than
	     * all nodes in its left subtree. Thus the modified tree is well
	     * formed. */

	    /* pnext is the address of the pointer which points to next (either
	     * parent's lchild or rchild) */
	    pnext = &dnode->rchild;
	    next = *pnext;

	    //Lock as you traverse down the tree
	    pthread_rwlock_rdlock(&(next->mutex_node_lock));

	    while (next->lchild != 0) 
	    {
	    	//Release the lock
	    	pthread_rwlock_unlock(&(next->mutex_node_lock));
		    /* work our way down the lchild chain, finding the smallest
		     * node in the subtree. */
		    pnext = &next->lchild;

		    next = *pnext;

		    pthread_rwlock_rdlock(&(next->mutex_node_lock));
    	}
    	//At teh end, the parent of the dnode is locked

    	pthread_rwlock_unlock(&(next->mutex_node_lock));
    	pthread_rwlock_wrlock(&(next->mutex_node_lock));

	    swap_pointers(&dnode->name, &next->name);
	    swap_pointers(&dnode->value, &next->value);
	    *pnext = next->rchild; //This part seems magical

	    pthread_rwlock_unlock(&(next->mutex_node_lock));

    	node_destroy(next);
	}

	pthread_rwlock_unlock(&(parent->mutex_node_lock));
	pthread_rwlock_unlock(&(dnode->mutex_node_lock));

	return 1;
}

/* Search the tree, starting at parent, for a node containing name (the "target
 * node").  Return a pointer to the node, if found, otherwise return 0.  If
 * parentpp is not 0, then it points to a location at which the address of the
 * parent of the target node is stored.  If the target node is not found, the
 * location pointed to by parentpp is set to what would be the the address of
 * the parent of the target node, if it were there.
 *
 * Assumptions:
 * parent is not null and it does not contain name */
 //USed for query
node_t *searchQ(char *name, node_t * parent, node_t ** parentpp) {

    node_t *next;
    node_t *result;

    //Lock the parent as you traverse down
    pthread_rwlock_rdlock(&(parent->mutex_node_lock));

    if (strcmp(name, parent->name) < 0) 
    {
    	next = parent->lchild;
    }
    else 
    {
    	next = parent->rchild;
    }

    //pthread_rwlock_lock(&(next->mutex_node_lock));

    if (next == NULL) 
    {
		result = NULL;
    } 
    else 
    {
		if (strcmp(name, next->name) == 0) 
		{
		    /* Note that this falls through to the if (parentpp .. ) statement
		     * below. */
			//Found it lock the Read
		    result = next;
		} 
		else 
		{
		    /* "We have to go deeper!" This recurses and returns from here
		     * after the recursion has returned result and set parentpp */
			//pthread_rwlock_unlock(&(next->mutex_node_lock));
			//Release the parentbefore recursing down
			pthread_rwlock_unlock(&(parent->mutex_node_lock));
		    result = searchQ(name, next, parentpp);
		    return result;
		}
    }

    /* record a parent if we are looking for one */
    if (parentpp != 0) 
    {
    	*parentpp = parent;
    	
    }

    //pthread_rwlock_unlock(&(parent->mutex_node_lock));
    if(result != NULL)
    {
    	pthread_rwlock_rdlock(&(result->mutex_node_lock));
    }

    return (result);
}

/* Search the tree, starting at parent, for a node containing name (the "target
 * node").  Return a pointer to the node, if found, otherwise return 0.  If
 * parentpp is not 0, then it points to a location at which the address of the
 * parent of the target node is stored.  If the target node is not found, the
 * location pointed to by parentpp is set to what would be the the address of
 * the parent of the target node, if it were there.
 *
 * Assumptions:
 * parent is not null and it does not contain name */
 //Used for Add and Remove
node_t *searchAR(char *name, node_t * parent, node_t ** parentpp) {
	//Same as SearchQ jsut with readlocks instead
    node_t *next;
    node_t *result;

    pthread_rwlock_wrlock(&(parent->mutex_node_lock));

    if (strcmp(name, parent->name) < 0) next = parent->lchild;
    else next = parent->rchild;

    if (next == NULL) 
    {
		result = NULL;
    } 
    else 
    {
		if (strcmp(name, next->name) == 0) 
		{
		    /* Note that this falls through to the if (parentpp .. ) statement
		     * below. */
		    result = next;
		} 
		else 
		{
		    /* "We have to go deeper!" This recurses and returns from here
		     * after the recursion has returned result and set parentpp */

			pthread_rwlock_unlock(&(parent->mutex_node_lock));
		    result = searchAR(name, next, parentpp);
		    return result;
		}
    }

    /* record a parent if we are looking for one */
    if (parentpp != 0) 
    {
    	*parentpp = parent;
    }

    if(result != NULL)
    {
    	pthread_rwlock_wrlock(&(result->mutex_node_lock));
    }

    return (result);
}


/* Search the tree, starting at parent, for a node containing name (the "target
 * node").  Return a pointer to the node, if found, otherwise return 0.  If
 * parentpp is not 0, then it points to a location at which the address of the
 * parent of the target node is stored.  If the target node is not found, the
 * location pointed to by parentpp is set to what would be the the address of
 * the parent of the target node, if it were there.
 *
 * Assumptions:
 * parent is not null and it does not contain name */
 //USed for Remove
//node_t *searchR(char *name, node_t * parent, node_t ** parentpp) {
//
//    node_t *next;
//    node_t *result;
//
//    pthread_rwlock_wrlock(&(parent->mutex_node_lock));
//
//   if (strcmp(name, parent->name) < 0) next = parent->lchild;
//    else next = parent->rchild;
//
//    if (next == NULL) 
//    {
//		result = NULL;
//    } 
//    else 
//    {
//		if (strcmp(name, next->name) == 0) 
//		{
//		    /* Note that this falls through to the if (parentpp .. ) statement
//		     * below. */
//		    result = next;
//		} 
//		else 
//		{
//		    /* "We have to go deeper!" This recurses and returns from here
//		     * after the recursion has returned result and set parentpp */
//			pthread_rwlock_unlock(&(parent->mutex_node_lock));
//		    result = searchR(name, next, parentpp);
//		    return result;
//		}
//    }
//
//  /* record a parent if we are looking for one */
//    if (parentpp != 0)
//    {
//		*parentpp = parent;
//	}
//
//	if(result != NULL)
//    {
//    	pthread_rwlock_wrlock(&(result->mutex_node_lock));
//    }
//
//    return (result);
//}


/*
 * Parse the command in command, execute it on the DB rooted at head and return
 * a string describing the results.  Response must be a writable string that
 * can hold len characters.  The response is stored in response.
 */
void interpret_command(char *command, char *response, int len)
{
    char value[256];
    char ibuf[256];
    char name[256];

    if (strlen(command) <= 1) {
	strncpy(response, "ill-formed command", len - 1);
	return;
    }

    switch (command[0]) {
    case 'q':
	/* Query */
	sscanf(&command[1], "%255s", name);
	if (strlen(name) == 0) {
	    strncpy(response, "ill-formed command", len - 1);
	    return;
	}

	query(name, response, len);
	if (strlen(response) == 0) {
	    strncpy(response, "not found", len - 1);
	}

	return;

    case 'a':
	/* Add to the database */
	sscanf(&command[1], "%255s %255s", name, value);
	if ((strlen(name) == 0) || (strlen(value) == 0)) {
	    strncpy(response, "ill-formed command", len - 1);
	    return;
	}

	if (add(name, value)) {
	    strncpy(response, "added", len - 1);
	} else {
	    strncpy(response, "already in database", len - 1);
	}

	return;

    case 'd':
	/* Delete from the database */
	sscanf(&command[1], "%255s", name);
	if (strlen(name) == 0) {
	    strncpy(response, "ill-formed command", len - 1);
	    return;
	}

	if (xremove(name)) {
	    strncpy(response, "removed", len - 1);
	} else {
	    strncpy(response, "not in database", len - 1);
	}

	    return;

    case 'f':
	/* process the commands in a file (silently) */
	sscanf(&command[1], "%255s", name);
	if (name[0] == '\0') {
	    strncpy(response, "ill-formed command", len - 1);
	    return;
	}

	{
	    FILE *finput = fopen(name, "r");
	    if (!finput) {
		strncpy(response, "bad file name", len - 1);
		return;
	    }
	    while (fgets(ibuf, sizeof(ibuf), finput) != 0) {
		interpret_command(ibuf, response, len);
	    }
	    fclose(finput);
	}
	strncpy(response, "file processed", len - 1);
	return;

    default:
	strncpy(response, "ill-formed command", len - 1);
	return;
    }
}
