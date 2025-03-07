/*
 * NewRMarkdownDialog.java
 *
 * Copyright (C) 2022 by Posit Software, PBC
 *
 * Unless you have received this program directly from Posit Software pursuant
 * to the terms of a commercial license agreement with Posit Software, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */
package org.rstudio.studio.client.workbench.views.source.editors.text.ui;

import java.util.ArrayList;
import java.util.List;

import com.google.gwt.aria.client.Roles;
import com.google.gwt.core.client.GWT;
import com.google.gwt.core.client.JavaScriptObject;
import com.google.gwt.core.client.JsArray;
import com.google.gwt.event.dom.client.ChangeEvent;
import com.google.gwt.event.dom.client.ChangeHandler;
import com.google.gwt.event.logical.shared.ValueChangeEvent;
import com.google.gwt.event.logical.shared.ValueChangeHandler;
import com.google.gwt.resources.client.ImageResource;
import com.google.gwt.safehtml.shared.SafeHtmlBuilder;
import com.google.gwt.uibinder.client.UiBinder;
import com.google.gwt.uibinder.client.UiFactory;
import com.google.gwt.uibinder.client.UiField;
import com.google.gwt.user.client.ui.CheckBox;
import com.google.gwt.user.client.ui.HTMLPanel;
import com.google.gwt.user.client.ui.Label;
import com.google.gwt.user.client.ui.RadioButton;
import com.google.gwt.user.client.ui.TextBox;
import com.google.gwt.user.client.ui.Widget;

import org.rstudio.core.client.ElementIds;
import org.rstudio.core.client.StringUtil;
import org.rstudio.core.client.files.FileSystemItem;
import org.rstudio.core.client.resources.ImageResource2x;
import org.rstudio.core.client.widget.ModalDialog;
import org.rstudio.core.client.widget.OperationWithInput;
import org.rstudio.core.client.widget.ThemedButton;
import org.rstudio.core.client.widget.WidgetListBox;
import org.rstudio.studio.client.RStudioGinjector;
import org.rstudio.studio.client.common.HelpLink;
import org.rstudio.studio.client.common.newdocument.NewDocumentResources;
import org.rstudio.studio.client.common.newdocument.TemplateMenuItem;
import org.rstudio.studio.client.rmarkdown.model.RMarkdownContext;
import org.rstudio.studio.client.rmarkdown.model.RMarkdownServerOperations;
import org.rstudio.studio.client.rmarkdown.model.RmdChosenTemplate;
import org.rstudio.studio.client.rmarkdown.model.RmdFrontMatter;
import org.rstudio.studio.client.rmarkdown.model.RmdOutputFormat;
import org.rstudio.studio.client.rmarkdown.model.RmdTemplate;
import org.rstudio.studio.client.rmarkdown.model.RmdTemplateData;
import org.rstudio.studio.client.rmarkdown.model.RmdTemplateFormat;
import org.rstudio.studio.client.rmarkdown.ui.RmdTemplateChooser;
import org.rstudio.studio.client.workbench.WorkbenchContext;
import org.rstudio.studio.client.workbench.model.SessionInfo;
import org.rstudio.studio.client.workbench.prefs.model.UserPrefs;
import org.rstudio.studio.client.workbench.views.source.ViewsSourceConstants;

public class NewRMarkdownDialog extends ModalDialog<NewRMarkdownDialog.Result>
{
   private static final ViewsSourceConstants constants_ = GWT.create(ViewsSourceConstants.class);
   public static class RmdNewDocument
   {  
      public RmdNewDocument(String template, String author, String title, String date, 
                            String format, boolean isShiny)
      {
         template_ = template;
         title_ = title;
         author_ = author;
         date_ = date;
         isShiny_ = isShiny;
         format_ = format;
         result_ = toJSO(author, title, date, format, isShiny);
      }
      
      public String getTemplate()
      {
         return template_;
      }
      
      public String getAuthor()
      {
         return author_;
      }
      
      public String getTitle()
      {
         return title_;
      }

      public String getDate()
      {
         return date_;
      }
      
      public boolean isShiny()
      {
         return isShiny_;
      }
      
      public String getFormat()
      {
         return format_;
      }
      
      public JavaScriptObject getJSOResult()
      {
         return result_;
      }
      
      private final JavaScriptObject toJSO(String author, 
                                           String title, 
                                           String date,
                                           String format, 
                                           boolean isShiny)
      {
         RmdFrontMatter result = RmdFrontMatter.create();
         result.applyCreateOptions(author, title, date, format, isShiny);
         return result;
      }

      private final String template_;
      private final String author_;
      private final String title_;
      private final String date_;
      private final boolean isShiny_;
      private final String format_;
      private final JavaScriptObject result_;
   }
   
   public static class Result
   {
      public Result (RmdNewDocument newDocument, RmdChosenTemplate fromTemplate,
                     boolean isNewDocument)
      {
         newDocument_ = newDocument;
         fromTemplate_ = fromTemplate;
         isNewDocument_ = isNewDocument;
      }
      
      public RmdNewDocument getNewDocument()
      {
         return newDocument_;
      }
      
      public RmdChosenTemplate getFromTemplate()
      {
         return fromTemplate_;
      }
      
      public boolean isNewDocument()
      {
         return isNewDocument_;
      }
      
      private final RmdNewDocument newDocument_;
      private final RmdChosenTemplate fromTemplate_;
      private final boolean isNewDocument_;
   }

   public interface Binder extends UiBinder<Widget, NewRMarkdownDialog>
   {
   }
   

   public NewRMarkdownDialog(
         RMarkdownServerOperations server,
         RMarkdownContext context,
         WorkbenchContext workbench,
         UserPrefs prefs,
         OperationWithInput<Result> operation)
   {
      super(constants_.newRMarkdown(), Roles.getDialogRole(), operation);
      server_ = server;
      context_ = context;
      templateChooser_ = new RmdTemplateChooser(server_);

      mainWidget_ = GWT.<Binder>create(Binder.class).createAndBindUi(this);
      formatOptions_ = new ArrayList<>();
      resources.styles().ensureInjected();
      txtAuthor_.setText(prefs.documentAuthor().getGlobalValue());
      txtTitle_.setText("Untitled"); //$NON-NLS-1$
      checkboxAutoDate_.setText(prefs.rmdAutoDate().getDescription());
      
      boolean auto = prefs.rmdAutoDate().getGlobalValue();
      txtDate_.setText(getISODate());
      txtDate_.setEnabled(!auto);

      checkboxAutoDate_.setValue(auto);
      checkboxAutoDate_.addValueChangeHandler(new ValueChangeHandler<Boolean>() {

         @Override
         public void onValueChange(ValueChangeEvent<Boolean> event) {
            Boolean auto = event.getValue();
            prefs.rmdAutoDate().setGlobalValue(auto);
            txtDate_.setEnabled(!auto);
         }
      });
      
      Roles.getListboxRole().setAriaLabelProperty(listTemplates_.getElement(), constants_.templatesCapitalized());
      listTemplates_.addChangeHandler(new ChangeHandler()
      {
         @Override
         public void onChange(ChangeEvent event)
         {
            updateOptions(getSelectedTemplate());
         }
      });

      templates_ = RmdTemplateData.getTemplates();
      for (int i = 0; i < templates_.length(); i++)
      {
         String templateName = templates_.get(i).getName();
         TemplateMenuItem menuItem = new TemplateMenuItem(templateName);
         
         ImageResource img = null;

         // Special treatment for built-in templates with known names
         if (templateName == RmdTemplateData.DOCUMENT_TEMPLATE)
         {
            img = new ImageResource2x(resources.documentIcon2x());
         } 
         else if (templateName == RmdTemplateData.PRESENTATION_TEMPLATE)
         {
            img = new ImageResource2x(resources.presentationIcon2x());
         }
         else
         {
            // don't advertise if no icon
            continue;
         }

         // Add an image if we have one
         if (img != null)
         {
            menuItem.addIcon(img);
         }

         listTemplates_.addItem(menuItem);
      }
      
      // Add the Shiny template
      TemplateMenuItem shinyItem = new TemplateMenuItem(TEMPLATE_SHINY);
      shinyItem.addIcon(new ImageResource2x(resources.shinyIcon2x()));
      listTemplates_.addItem(shinyItem);
       
      // Add the "From Template" item at the end of the list
      TemplateMenuItem templateItem = 
            new TemplateMenuItem(TEMPLATE_CHOOSE_EXISTING);
      templateItem.addIcon(new ImageResource2x(resources.templateIcon2x()));
      listTemplates_.addItem(templateItem);
      
      // Save templates to the current project directory if available, and the
      // current working directory if not
      FileSystemItem dir = workbench.getActiveProjectDir();
      if (dir == null)
         dir = workbench.getCurrentWorkingDir();
      templateChooser_.setTargetDirectory(dir.getPath());

      updateOptions(getSelectedTemplate());
      
      // Add option to create empty document
      ThemedButton emptyDoc = new ThemedButton(constants_.createEmptyDocument(), evt ->
      {
         closeDialog();
         if (operation != null)
            operation.execute(null);
         onSuccess();
      }); 
      addLeftButton(emptyDoc, ElementIds.EMPTY_DOC_BUTTON);
   }
   
   @UiFactory
   public HelpLink makeHelpCaption()
   {
      return new HelpLink(constants_.usingShinyWithRMarkdown(),
                          "using_rmarkdown_shiny");
   }
   
   @Override
   protected void focusInitialControl()
   {
      // when dialog is finished booting, focus the title so it's ready to
      // accept input
      txtTitle_.setSelectionRange(0, txtTitle_.getText().length());
      txtTitle_.setFocus(true);
   }

   @Override
   protected Result collectInput()
   {
      String formatName = "";
      boolean isShiny = getSelectedTemplate() == TEMPLATE_SHINY;
      for (int i = 0; i < formatOptions_.size(); i++)
      {
         if (formatOptions_.get(i).getValue())
         {
            // for Shiny documents, manually choose the underlying format to
            // represent the document
            if (isShiny)
            {
               String option = formatOptions_.get(i).getText();
               if (option == SHINY_DOC_NAME)
                  formatName = "html_document";
               else if (option == SHINY_PRESENTATION_NAME)
                  formatName = "ioslides_presentation";
            }
            // for other documents, read the format from the template
            else
            {
               formatName = currentTemplate_.getFormats().get(i).getName();
            }
            break;
         }
      }
      return new Result(
            new RmdNewDocument(getSelectedTemplate(), 
                               txtAuthor_.getText().trim(), 
                               txtTitle_.getText().trim(), 
                               checkboxAutoDate_.getValue() ? "`r Sys.Date()`" : txtDate_.getText().trim(),
                               formatName,
                               isShiny),
            templateChooser_.getChosenTemplate(),
            getSelectedTemplate() != TEMPLATE_CHOOSE_EXISTING);
   }

   @Override
   protected boolean validate(Result input)
   {
      // the dialog isn't valid if the user's chosen to create a document from
      // a template but hasn't chosen a template. 
      if (input.isNewDocument() || 
          input.getFromTemplate().getTemplatePath() != null)
         return true;
      return false;
   }

   @Override
   protected Widget createMainWidget()
   {
      return mainWidget_;
   }
   
   private String getSelectedTemplate() 
   {
      int idx = listTemplates_.getSelectedIndex();
      TemplateMenuItem item = listTemplates_.getItemAtIdx(idx);
      if (item.getName() == TEMPLATE_CHOOSE_EXISTING ||
          item.getName() == TEMPLATE_SHINY)
         return item.getName();
      else
         return templates_.get(idx).getName();
   }
   
   private void updateOptions(String selectedTemplate)
   {
      boolean existing = selectedTemplate == TEMPLATE_CHOOSE_EXISTING;
      boolean shiny = selectedTemplate == TEMPLATE_SHINY;

      // toggle visibility of UI elements based on which section of the dialog
      // we're in 
      newTemplatePanel_.setVisible(!existing);
      existingTemplatePanel_.setVisible(existing);
      shinyInfoPanel_.setVisible(shiny);
      
      if (existing)
      {
         if (templateChooser_.getState() == RmdTemplateChooser.STATE_EMPTY)
         {
            populateTemplates();
         }
         return;
      }
      
      templateFormatPanel_.clear();
      formatOptions_.clear();
      
      if (shiny)
      {
         templateFormatPanel_.add(createFormatOption(SHINY_DOC_NAME, 
               constants_.shinyDocNameDescription()));
         templateFormatPanel_.add(createFormatOption(SHINY_PRESENTATION_NAME, 
               constants_.shinyPresentationNameDescription()));
      }
      else 
      {
         
         currentTemplate_ = RmdTemplate.getTemplate(templates_, selectedTemplate);
         if (currentTemplate_ == null)
            return;
         
         // Add each format to the dialog
         JsArray<RmdTemplateFormat> formats = currentTemplate_.getFormats();
         for (int i = 0; i < formats.length(); i++)
         {
            Widget option = createFormatOption(formats.get(i));
            
            // hide if no notes
            if (StringUtil.isNullOrEmpty(formats.get(i).getNotes()))
               option.setVisible(false);
            SessionInfo sessionInfo = 
                  RStudioGinjector.INSTANCE.getSession().getSessionInfo();
            
            // hide if not supported yet
            if (sessionInfo != null &&
                !sessionInfo.getPptAvailable() &&
                formats.get(i).getName() == RmdOutputFormat.OUTPUT_PPT_PRESENTATION)
            {
               option.setVisible(false);
            }

            templateFormatPanel_.add(option);
         }
      }
         
      // select the first visible format by default
      for (int i = 0; i < formatOptions_.size(); i++)
      {
         if (formatOptions_.get(i).getParent().isVisible())
         {
            formatOptions_.get(i).setValue(true);
            break;
         }
      }
   }
   
   private native String getISODate() /*-{
      return new Date().toISOString().substring(0, 10);
   }-*/;

   private void populateTemplates()
   {
      templateChooser_.populateTemplates();
   }

   private Widget createFormatOption(RmdTemplateFormat format)
   {
      return createFormatOption(format.getUiName(), format.getNotes());
   }
   
   private Widget createFormatOption(String name, String description)
   {
      HTMLPanel formatWrapper = new HTMLPanel("");
      formatWrapper.setStyleName(resources.styles().outputFormat());
      SafeHtmlBuilder sb = new SafeHtmlBuilder();
      sb.appendHtmlConstant("<span class=\"" + resources.styles().outputFormatName() + 
                            "\">");
      sb.appendEscaped(name);
      sb.appendHtmlConstant("</span>");
      RadioButton button = new RadioButton("DefaultOutputFormat", 
                                           sb.toSafeHtml().asString(), true);
      button.addStyleName(resources.styles().outputFormatChoice());
      formatOptions_.add(button);
      formatWrapper.add(button);
      Label label = new Label(description);
      label.setStyleName(resources.styles().outputFormatDetails());
      formatWrapper.add(label);
      return formatWrapper;
   }
   
   @UiField TextBox txtAuthor_;
   @UiField TextBox txtTitle_;
   @UiField TextBox txtDate_;
   @UiField WidgetListBox<TemplateMenuItem> listTemplates_;
   @UiField NewDocumentResources resources;
   @UiField HTMLPanel templateFormatPanel_;
   @UiField HTMLPanel newTemplatePanel_;
   @UiField HTMLPanel existingTemplatePanel_;
   @UiField(provided=true) RmdTemplateChooser templateChooser_;
   @UiField HTMLPanel shinyInfoPanel_;
   @UiField CheckBox checkboxAutoDate_;
   
   private final Widget mainWidget_;
   private List<RadioButton> formatOptions_;
   private JsArray<RmdTemplate> templates_;
   private RmdTemplate currentTemplate_;

   @SuppressWarnings("unused")
   private final RMarkdownContext context_;
   private final RMarkdownServerOperations server_;
   
   private final static String TEMPLATE_CHOOSE_EXISTING = constants_.fromTemplate();
   private final static String TEMPLATE_SHINY = constants_.shinyCapitalized();
   private final static String SHINY_DOC_NAME = constants_.shinyDocument();
   private final static String SHINY_PRESENTATION_NAME = constants_.shinyPresentation();
}
