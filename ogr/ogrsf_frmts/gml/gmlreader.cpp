/******************************************************************************
 * $Id$
 *
 * Project:  GML Reader
 * Purpose:  Implementation of GMLReader class.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2002, Frank Warmerdam
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************
 *
 * $Log$
 * Revision 1.2  2002/01/24 17:37:33  warmerda
 * added to/from XML support
 *
 * Revision 1.1  2002/01/04 19:46:30  warmerda
 * New
 */

#include "gmlreaderp.h"
#include "cpl_conv.h"

/************************************************************************/
/*                          CreateGMLReader()                           */
/************************************************************************/

IGMLReader *CreateGMLReader()

{
    return new GMLReader();
}

/************************************************************************/
/*                            ~IGMLReader()                             */
/************************************************************************/

IGMLReader::~IGMLReader()

{
}

/************************************************************************/
/*                             GMLReader()                              */
/************************************************************************/

GMLReader::GMLReader()

{
    m_nClassCount = 0;
    m_papoClass = NULL;

    m_poGMLHandler = NULL;
    m_poSAXReader = NULL;
    m_bReadStarted = FALSE;
    
    m_poState = NULL;
    m_poCompleteFeature = NULL;
}

/************************************************************************/
/*                             ~GMLReader()                             */
/************************************************************************/

GMLReader::~GMLReader()

{
    ClearClasses();

    CPLFree( m_pszFilename );

    CleanupParser();
}

/************************************************************************/
/*                             SetSource()                              */
/************************************************************************/

void GMLReader::SetSourceFile( const char *pszFilename )

{
    CPLFree( m_pszFilename );
    m_pszFilename = CPLStrdup( pszFilename );
}

/************************************************************************/
/*                            SetupParser()                             */
/************************************************************************/

int GMLReader::SetupParser()

{
    static int bXercesInitialized = FALSE;

    if( !bXercesInitialized )
    {
        try
        {
            XMLPlatformUtils::Initialize();
        }
        
        catch (const XMLException& toCatch)
        {
            printf( "Error during initialization! Message:\n%s\n", 
                    tr_strdup(toCatch.getMessage()) );
            return FALSE;
        }
        bXercesInitialized = TRUE;
    }

    // Cleanup any old parser.
    if( m_poSAXReader != NULL )
        CleanupParser();

    // Create and initialize parser.
    m_poSAXReader = XMLReaderFactory::createXMLReader();
    
    m_poGMLHandler = new GMLHandler( this );

    m_poSAXReader->setContentHandler( m_poGMLHandler );
    m_poSAXReader->setErrorHandler( m_poGMLHandler );
    m_poSAXReader->setLexicalHandler( m_poGMLHandler );
    m_poSAXReader->setEntityResolver( m_poGMLHandler );
    m_poSAXReader->setDTDHandler( m_poGMLHandler );

    m_poSAXReader->setFeature(
        XMLString::transcode("http://xml.org/sax/features/validation"), true);
    m_poSAXReader->setFeature(
        XMLString::transcode("http://xml.org/sax/features/namespaces"), true);

    m_bReadStarted = FALSE;

    // Push an empty state.
    PushState( new GMLReadState() );

    return TRUE;
}

/************************************************************************/
/*                           CleanupParser()                            */
/************************************************************************/

void GMLReader::CleanupParser()

{
    if( m_poSAXReader == NULL )
        return;

    while( m_poState )
        PopState();

    delete m_poSAXReader;
    m_poSAXReader = NULL;

    delete m_poGMLHandler;
    m_poGMLHandler = NULL;
}

/************************************************************************/
/*                            NextFeature()                             */
/************************************************************************/

GMLFeature *GMLReader::NextFeature()

{
    GMLFeature *poReturn = NULL;

    try
    {
        if( !m_bReadStarted )
        {
            if( m_poSAXReader == NULL )
                SetupParser();

            if( !m_poSAXReader->parseFirst( m_pszFilename, m_oToFill ) )
                return NULL;
            m_bReadStarted = TRUE;
        }

        while( m_poCompleteFeature == NULL 
               && m_poSAXReader->parseNext( m_oToFill ) ) {}

        poReturn = m_poCompleteFeature;
        m_poCompleteFeature = NULL;

    }
    catch (const XMLException& toCatch)
    {
        printf( "Error during NextFeature()! Message:\n%s\n", 
                tr_strdup( toCatch.getMessage() ) );
    }

    return poReturn;
}

/************************************************************************/
/*                            PushFeature()                             */
/*                                                                      */
/*      Create a feature based on the named element.  If the            */
/*      corresponding feature class doesn't exist yet, then create      */
/*      it now.  A new GMLReadState will be created for the feature,    */
/*      and it will be placed within that state.  The state is          */
/*      pushed onto the readstate stack.                                */
/************************************************************************/

void GMLReader::PushFeature( const char *pszElement, 
                             const Attributes &attrs )

{
    int	iClass;

/* -------------------------------------------------------------------- */
/*      Find the class of this element.                                 */
/* -------------------------------------------------------------------- */
    for( iClass = 0; iClass < GetClassCount(); iClass++ )
    {
        if( EQUAL(pszElement,GetClass(iClass)->GetElementName()) )
            break;
    }

/* -------------------------------------------------------------------- */
/*      Create a new feature class for this element, if there is no     */
/*      existing class for it.                                          */
/* -------------------------------------------------------------------- */
    if( iClass == GetClassCount() )
    {
        CPLAssert( !IsClassListLocked() );

        GMLFeatureClass *poNewClass = new GMLFeatureClass( pszElement );

        AddClass( poNewClass );
    }

/* -------------------------------------------------------------------- */
/*      Create a feature of this feature class.  Try to set the fid     */
/*      if available.                                                   */
/* -------------------------------------------------------------------- */
    GMLFeature *poFeature = new GMLFeature( GetClass( iClass ) );
    int	nFIDIndex;
    XMLCh   anFID[100];

    tr_strcpy( anFID, "fid" );
    nFIDIndex = attrs.getIndex( anFID );
    if( nFIDIndex != -1 )
    {
        char *pszFID = tr_strdup( attrs.getValue( nFIDIndex ) );
        poFeature->SetFID( pszFID );
        CPLFree( pszFID );
    }

/* -------------------------------------------------------------------- */
/*      Create and push a new read state.                               */
/* -------------------------------------------------------------------- */
    GMLReadState *poState;

    poState = new GMLReadState();
    poState->m_poFeature = poFeature;
    PushState( poState );
}

/************************************************************************/
/*                          IsFeatureElement()                          */
/*                                                                      */
/*      Based on context and the element name, is this element a new    */
/*      GML feature element?                                            */
/************************************************************************/

int GMLReader::IsFeatureElement( const char *pszElement )

{
    CPLAssert( m_poState != NULL );

    if( !EQUAL(m_poState->GetLastComponent(),"gml:featureMember") )
        return FALSE;						

    // If the class list isn't locked, any element that is a featureMember
    // will do. 
    if( !IsClassListLocked() )
        return TRUE;

    // otherwise, find a class with the desired element name.
    for( int i = 0; i < GetClassCount(); i++ )
    {
        if( EQUAL(pszElement,GetClass(i)->GetElementName()) )
            return TRUE;
    }

    return FALSE;
}

/************************************************************************/
/*                         IsAttributeElement()                         */
/************************************************************************/

int GMLReader::IsAttributeElement( const char *pszElement )

{
    if( m_poState->m_poFeature == NULL )
        return FALSE;

    if( m_poState->m_nPathLength > 0 )
        return FALSE;

    GMLFeatureClass *poClass = m_poState->m_poFeature->GetClass();

    if( !poClass->IsSchemaLocked() )
        return TRUE;

    for( int i = 0; i < poClass->GetPropertyCount(); i++ )
        if( EQUAL(poClass->GetProperty(i)->GetSrcElement(),pszElement) )
            return TRUE;

    return FALSE;
}

/************************************************************************/
/*                              PopState()                              */
/************************************************************************/

void GMLReader::PopState()

{
    if( m_poState != NULL )
    {
        if( m_poState->m_poFeature != NULL && m_poCompleteFeature == NULL )
        {
            m_poCompleteFeature = m_poState->m_poFeature;
            m_poState->m_poFeature = NULL;
        }

        GMLReadState *poParent;

        poParent = m_poState->m_poParentState;
        
        delete m_poState;
        m_poState = poParent;
    }
}

/************************************************************************/
/*                             PushState()                              */
/************************************************************************/

void GMLReader::PushState( GMLReadState *poState )

{
    poState->m_poParentState = m_poState;
    m_poState = poState;
}

/************************************************************************/
/*                              GetClass()                              */
/************************************************************************/

GMLFeatureClass *GMLReader::GetClass( int iClass ) const

{
    if( iClass < 0 || iClass >= m_nClassCount )
        return NULL;
    else
        return m_papoClass[iClass];
}

/************************************************************************/
/*                              GetClass()                              */
/************************************************************************/

GMLFeatureClass *GMLReader::GetClass( const char *pszName ) const

{
    for( int iClass = 0; iClass < m_nClassCount; iClass++ )
    {
        if( EQUAL(GetClass(iClass)->GetName(),pszName) )
            return GetClass(iClass);
    }

    return NULL;
}

/************************************************************************/
/*                              AddClass()                              */
/************************************************************************/

int GMLReader::AddClass( GMLFeatureClass *poNewClass )

{
    CPLAssert( GetClass( poNewClass->GetName() ) == NULL );

    m_nClassCount++;
    m_papoClass = (GMLFeatureClass **) 
        CPLRealloc( m_papoClass, sizeof(void*) * m_nClassCount );
    m_papoClass[m_nClassCount-1] = poNewClass;

    return m_nClassCount-1;
}

/************************************************************************/
/*                            ClearClasses()                            */
/************************************************************************/

void GMLReader::ClearClasses()

{
    for( int i = 0; i < m_nClassCount; i++ )
        delete m_papoClass[i];
    CPLFree( m_papoClass );

    m_nClassCount = 0;
    m_papoClass = NULL;
}

/************************************************************************/
/*                         SetFeatureProperty()                         */
/*                                                                      */
/*      Set the property value on the current feature, adding the       */
/*      property name to the GMLFeatureClass if required.               */
/*      Eventually this function may also "refine" the property         */
/*      type based on what is encountered.                              */
/************************************************************************/

void GMLReader::SetFeatureProperty( const char *pszElement, 
                                    const char *pszValue )

{
    GMLFeature *poFeature = GetState()->m_poFeature;

    CPLAssert( poFeature  != NULL );

/* -------------------------------------------------------------------- */
/*      Does this property exist in the feature class?  If not, add     */
/*      it.                                                             */
/* -------------------------------------------------------------------- */
    GMLFeatureClass *poClass = poFeature->GetClass();
    int      iProperty;

    for( iProperty=0; iProperty < poClass->GetPropertyCount(); iProperty++ )
    {
        if( EQUAL(poClass->GetProperty( iProperty )->GetSrcElement(),
                  pszElement ) )
            break;
    }
    
    if( iProperty == poClass->GetPropertyCount() )
    {
        CPLAssert( !poClass->IsSchemaLocked() );

        poClass->AddProperty( new GMLPropertyDefn( pszElement, pszElement ) );
    }

/* -------------------------------------------------------------------- */
/*      Set the property                                                */
/* -------------------------------------------------------------------- */
    poFeature->SetProperty( iProperty, pszValue );
}

/************************************************************************/
/*                            LoadClasses()                             */
/************************************************************************/

int GMLReader::LoadClasses( const char *pszFile )

{
    // Add logic later to determine reasonable default schema file. 
    if( pszFile == NULL )
        return FALSE;

/* -------------------------------------------------------------------- */
/*      Load the raw XML file.                                          */
/* -------------------------------------------------------------------- */
    FILE       *fp;
    int		nLength;
    char        *pszWholeText;

    fp = VSIFOpen( pszFile, "r" );

    if( fp == NULL )
    {
        CPLError( CE_Failure, CPLE_OpenFailed, 
                  "Failed to open file %s.", pszFile );
        return FALSE;
    }

    VSIFSeek( fp, 0, SEEK_END );
    nLength = VSIFTell( fp );
    VSIFSeek( fp, 0, SEEK_SET );

    pszWholeText = (char *) VSIMalloc(nLength+1);
    if( pszWholeText == NULL )
    {
        CPLError( CE_Failure, CPLE_AppDefined, 
                  "Failed to allocate %d byte buffer for %s,\n"
                  "is this really a GMLFeatureClassList file?",
                  nLength, pszFile );
        VSIFClose( fp );
        return FALSE;
    }
    
    if( VSIFRead( pszWholeText, nLength, 1, fp ) != 1 )
    {
        VSIFree( pszWholeText );
        VSIFClose( fp );
        CPLError( CE_Failure, CPLE_AppDefined, 
                  "Read failed on %s.", pszFile );
        return FALSE;
    }
    pszWholeText[nLength] = '\0';

    VSIFClose( fp );

    if( strstr( pszWholeText, "<GMLFeatureClassList>" ) == NULL )
    {
        VSIFree( pszWholeText );
        CPLError( CE_Failure, CPLE_AppDefined, 
                  "File %s does not contain a GMLFeatureClassList tree.",
                  pszFile );
        return FALSE;
    }

/* -------------------------------------------------------------------- */
/*      Convert to XML parse tree.                                      */
/* -------------------------------------------------------------------- */
    CPLXMLNode *psRoot;

    psRoot = CPLParseXMLString( pszWholeText );
    VSIFree( pszWholeText );

    // We assume parser will report errors via CPL.
    if( psRoot == NULL )
        return FALSE;

    if( psRoot->eType != CXT_Element 
        || !EQUAL(psRoot->pszValue,"GMLFeatureClassList") )
    {
        CPLDestroyXMLNode(psRoot);
        CPLError( CE_Failure, CPLE_AppDefined, 
                  "File %s is not a GMLFeatureClassList document.",
                  pszFile );
        return FALSE;
    }

/* -------------------------------------------------------------------- */
/*      Extract feature classes for all definitions found.              */
/* -------------------------------------------------------------------- */
    CPLXMLNode *psThis;

    for( psThis = psRoot->psChild; psThis != NULL; psThis = psThis->psNext )
    {
        if( psThis->eType == CXT_Element 
            && EQUAL(psThis->pszValue,"GMLFeatureClass") )
        {
            GMLFeatureClass   *poClass;

            poClass = new GMLFeatureClass();

            if( !poClass->InitializeFromXML( psThis ) )
            {
                delete poClass;
                CPLDestroyXMLNode( psRoot );
                return FALSE;
            }

            poClass->SetSchemaLocked( TRUE );

            AddClass( poClass );
        }
    }

    CPLDestroyXMLNode( psRoot );
    
    SetClassListLocked( TRUE );

    return TRUE;
}

/************************************************************************/
/*                            SaveClasses()                             */
/************************************************************************/

int GMLReader::SaveClasses( const char *pszFile )

{
    // Add logic later to determine reasonable default schema file. 
    if( pszFile == NULL )
        return FALSE;

/* -------------------------------------------------------------------- */
/*      Create in memory schema tree.                                   */
/* -------------------------------------------------------------------- */
    CPLXMLNode *psRoot;

    psRoot = CPLCreateXMLNode( NULL, CXT_Element, "GMLFeatureClassList" );

    for( int iClass = 0; iClass < GetClassCount(); iClass++ )
    {
        GMLFeatureClass *poClass = GetClass( iClass );
        
        CPLAddXMLChild( psRoot, poClass->SerializeToXML() );
    }

/* -------------------------------------------------------------------- */
/*      Serialize to disk.                                              */
/* -------------------------------------------------------------------- */
    FILE	*fp;
    int	        bSuccess = TRUE;
    char        *pszWholeText = CPLSerializeXMLTree( psRoot );
    
    CPLDestroyXMLNode( psRoot );
 
    fp = VSIFOpen( pszFile, "w" );
    
    if( fp == NULL )
        bSuccess = FALSE;
    else if( VSIFWrite( pszWholeText, strlen(pszWholeText), 1, fp ) != 1 )
        bSuccess = FALSE;
    else
        VSIFClose( fp );

    CPLFree( pszWholeText );

    return bSuccess;
}

