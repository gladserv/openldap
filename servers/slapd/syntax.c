/* syntax.c - routines to manage syntax definitions */
/* $OpenLDAP$ */
/*
 * Copyright 1998-2003 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

#include "portable.h"

#include <stdio.h>

#include <ac/ctype.h>
#include <ac/string.h>
#include <ac/socket.h>

#include "slap.h"
#include "ldap_pvt.h"

struct sindexrec {
	char		*sir_name;
	Syntax		*sir_syn;
};

static Avlnode	*syn_index = NULL;
static LDAP_SLIST_HEAD(SyntaxList, slap_syntax) syn_list
	= LDAP_SLIST_HEAD_INITIALIZER(&syn_list);

static int
syn_index_cmp(
	const void *v_sir1,
	const void *v_sir2
)
{
	const struct sindexrec *sir1 = v_sir1, *sir2 = v_sir2;
	return (strcmp( sir1->sir_name, sir2->sir_name ));
}

static int
syn_index_name_cmp(
	const void *name,
	const void *sir
)
{
	return (strcmp( name, ((const struct sindexrec *)sir)->sir_name ));
}

Syntax *
syn_find( const char *synname )
{
	struct sindexrec	*sir = NULL;

	if ( (sir = avl_find( syn_index, synname, syn_index_name_cmp )) != NULL ) {
		return( sir->sir_syn );
	}
	return( NULL );
}

Syntax *
syn_find_desc( const char *syndesc, int *len )
{
	Syntax		*synp;

	LDAP_SLIST_FOREACH(synp, &syn_list, ssyn_next) {
		if ((*len = dscompare( synp->ssyn_syn.syn_desc, syndesc, '{' /*'}'*/ ))) {
			return synp;
		}
	}
	return( NULL );
}

void
syn_destroy( void )
{
	Syntax *s;

	avl_free(syn_index, ldap_memfree);
	while( !LDAP_SLIST_EMPTY(&syn_list) ) {
		s = LDAP_SLIST_FIRST(&syn_list);
		LDAP_SLIST_REMOVE_HEAD(&syn_list, ssyn_next);
		ldap_syntax_free((LDAPSyntax *)s);
	}
}

static int
syn_insert(
    Syntax		*ssyn,
    const char		**err
)
{
	struct sindexrec	*sir;

	LDAP_SLIST_INSERT_HEAD( &syn_list, ssyn, ssyn_next );
 
	if ( ssyn->ssyn_oid ) {
		sir = (struct sindexrec *)
			SLAP_CALLOC( 1, sizeof(struct sindexrec) );
		if( sir == NULL ) {
#ifdef NEW_LOGGING
			LDAP_LOG( OPERATION, ERR, 
				"syn_insert: SLAP_CALLOC Error\n", 0, 0, 0 );
#else
			Debug( LDAP_DEBUG_ANY, "SLAP_CALLOC Error\n", 0, 0, 0 );
#endif
			return LDAP_OTHER;
		}
		sir->sir_name = ssyn->ssyn_oid;
		sir->sir_syn = ssyn;
		if ( avl_insert( &syn_index, (caddr_t) sir,
		                 syn_index_cmp, avl_dup_error ) ) {
			*err = ssyn->ssyn_oid;
			ldap_memfree(sir);
			return SLAP_SCHERR_SYN_DUP;
		}
		/* FIX: temporal consistency check */
		syn_find(sir->sir_name);
	}
	return 0;
}

int
syn_add(
    LDAPSyntax		*syn,
    slap_syntax_defs_rec *def,
    const char		**err
)
{
	Syntax		*ssyn;
	int		code;

	ssyn = (Syntax *) SLAP_CALLOC( 1, sizeof(Syntax) );
	if( ssyn == NULL ) {
#ifdef NEW_LOGGING
		LDAP_LOG( OPERATION, ERR, 
			"syn_add: SLAP_CALLOC Error\n", 0, 0, 0 );
#else
		Debug( LDAP_DEBUG_ANY, "SLAP_CALLOC Error\n", 0, 0, 0 );
#endif
		return LDAP_OTHER;
	}

	AC_MEMCPY( &ssyn->ssyn_syn, syn, sizeof(LDAPSyntax) );

	LDAP_SLIST_NEXT(ssyn,ssyn_next) = NULL;

	/*
	 * note: ssyn_bvoid uses the same memory of ssyn_syn.syn_oid;
	 * ssyn_oidlen is #defined as ssyn_bvoid.bv_len
	 */
	ssyn->ssyn_bvoid.bv_val = ssyn->ssyn_syn.syn_oid;
	ssyn->ssyn_oidlen = strlen(syn->syn_oid);
	ssyn->ssyn_flags = def->sd_flags;
	ssyn->ssyn_validate = def->sd_validate;
	ssyn->ssyn_normalize = def->sd_normalize;
	ssyn->ssyn_pretty = def->sd_pretty;

#ifdef SLAPD_BINARY_CONVERSION
	ssyn->ssyn_ber2str = def->sd_ber2str;
	ssyn->ssyn_str2ber = def->sd_str2ber;
#endif

	code = syn_insert(ssyn, err);
	return code;
}

int
register_syntax(
	slap_syntax_defs_rec *def )
{
	LDAPSyntax	*syn;
	int		code;
	const char	*err;

	syn = ldap_str2syntax( def->sd_desc, &code, &err, LDAP_SCHEMA_ALLOW_ALL);
	if ( !syn ) {
#ifdef NEW_LOGGING
		LDAP_LOG( CONFIG, ERR, 
			"register_syntax: Error - %s before %s in %s.\n",
			ldap_scherr2str(code), err, def->sd_desc );
#else
		Debug( LDAP_DEBUG_ANY, "Error in register_syntax: %s before %s in %s\n",
		    ldap_scherr2str(code), err, def->sd_desc );
#endif

		return( -1 );
	}

	code = syn_add( syn, def, &err );

	ldap_memfree( syn );

	if ( code ) {
#ifdef NEW_LOGGING
		LDAP_LOG( CONFIG, ERR, 
			"register_syntax: Error - %s %s in %s\n", 
			scherr2str(code), err, def->sd_desc );
#else
		Debug( LDAP_DEBUG_ANY, "Error in register_syntax: %s %s in %s\n",
		    scherr2str(code), err, def->sd_desc );
#endif

		return( -1 );
	}

	return( 0 );
}

int
syn_schema_info( Entry *e )
{
	struct berval	vals[2];
	Syntax		*syn;

	AttributeDescription *ad_ldapSyntaxes = slap_schema.si_ad_ldapSyntaxes;

	vals[1].bv_val = NULL;

	LDAP_SLIST_FOREACH(syn, &syn_list, ssyn_next ) {
		if ( ! syn->ssyn_validate ) {
			/* skip syntaxes without validators */
			continue;
		}
		if ( syn->ssyn_flags & SLAP_SYNTAX_HIDE ) {
			/* hide syntaxes */
			continue;
		}

		if ( ldap_syntax2bv( &syn->ssyn_syn, vals ) == NULL ) {
			return -1;
		}
#if 0
#ifdef NEW_LOGGING
		LDAP_LOG( config, ENTRY,
			   "syn_schema_info: Merging syn [%ld] %s\n",
			   (long)vals[0].bv_len, vals[0].bv_val, 0 );
#else
		Debug( LDAP_DEBUG_TRACE, "Merging syn [%ld] %s\n",
	       (long) vals[0].bv_len, vals[0].bv_val, 0 );
#endif

#endif
#ifdef SLAP_NVALUES
		if( attr_merge( e, ad_ldapSyntaxes, vals, NULL /* FIXME */ ) )
#else
		if( attr_merge( e, ad_ldapSyntaxes, vals ) )
#endif
		{
			return -1;
		}
		ldap_memfree( vals[0].bv_val );
	}
	return 0;
}

