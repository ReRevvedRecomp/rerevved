@{
    Module    = @{
        ModuleName      = 'PSScriptAnalyzer'
        RequiredVersion = '1.25.0'
    }
    Formatter = @{
        IncludeRules = @(
            'PSPlaceOpenBrace'
            'PSPlaceCloseBrace'
            'PSUseConsistentIndentation'
            'PSUseConsistentWhitespace'
            'PSAlignAssignmentStatement'
        )
        Rules        = @{
            PSPlaceOpenBrace           = @{
                Enable             = $true
                OnSameLine         = $true
                NewLineAfter       = $true
                IgnoreOneLineBlock = $true
            }
            PSPlaceCloseBrace          = @{
                Enable             = $true
                NewLineAfter       = $false
                IgnoreOneLineBlock = $true
                NoEmptyLineBefore  = $false
            }
            PSUseConsistentIndentation = @{
                Enable              = $true
                Kind                = 'space'
                IndentationSize     = 4
                PipelineIndentation = 'IncreaseIndentationForFirstPipeline'
            }
            PSUseConsistentWhitespace  = @{
                Enable                                  = $true
                CheckInnerBrace                         = $true
                CheckOpenBrace                          = $true
                CheckOpenParen                          = $true
                CheckOperator                           = $true
                CheckPipe                               = $true
                CheckPipeForRedundantWhitespace         = $false
                CheckSeparator                          = $true
                CheckParameter                          = $false
                IgnoreAssignmentOperatorInsideHashTable = $true
            }
            PSAlignAssignmentStatement = @{
                Enable         = $true
                CheckHashtable = $true
            }
        }
    }
    Analyzer  = @{
        Severity     = @('Error', 'Warning')
        IncludeRules = @(
            'PSAvoidAssignmentToAutomaticVariable'
            'PSPossibleIncorrectComparisonWithNull'
            'PSAvoidUsingInvokeExpression'
            'PSUseDeclaredVarsMoreThanAssignments'
        )
    }
}
